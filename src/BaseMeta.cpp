#include <sys/mman.h>

#include "BaseMeta.hpp"

using namespace std;
Sizeclass::Sizeclass(uint64_t thread_num,
		unsigned int bs, 
		unsigned int sbs, 
		MichaelScottQueue<Descriptor*>* pdq):
	partial_desc(pdq),
	sz(bs), 
	sbsize(sbs) {
	if(partial_desc == nullptr) {
		partial_desc = 
		new MichaelScottQueue<Descriptor*>(thread_num);
	}
	FLUSH(&partial_desc);
	FLUSH(&sz);
	FLUSH(&sbsize);
	FLUSHFENCE;
}

void Sizeclass::reinit_msq(uint64_t thread_num){
	if(partial_desc != nullptr) {
		delete partial_desc;
		partial_desc = 
		new MichaelScottQueue<Descriptor*>(thread_num);
		FLUSH(&partial_desc);
		FLUSHFENCE;
	}
}

BaseMeta::BaseMeta(RegionManager* m, uint64_t thd_num) : 
	mgr(m),
	free_desc(thd_num),
	free_sb(thd_num),
	thread_num(thd_num) {
	FLUSH(&thread_num);
	/* allocate these persistent data into specific memory address */
	/* TODO: metadata init */

	/* persistent roots init */
	for(int i=0;i<MAX_ROOTS;i++){
		roots[i]=nullptr;
		FLUSH(&roots[i]);
	}

	/* sizeclass init */
	for(int i=0;i<MAX_SMALLSIZE/GRANULARITY;i++){
		sizeclasses[i].reinit_msq(thd_num);
		sizeclasses[i].sz = (i+1)*GRANULARITY;
		FLUSH(&sizeclasses[i]);
	}
	sizeclasses[MAX_SMALLSIZE/GRANULARITY].reinit_msq(thd_num);
	sizeclasses[MAX_SMALLSIZE/GRANULARITY].sz = 0;
	sizeclasses[MAX_SMALLSIZE/GRANULARITY].sbsize = 0;
	FLUSH(&sizeclasses[MAX_SMALLSIZE/GRANULARITY]);//the size class for large blocks

	/* processor heap init */
	for(int t=0;t<PROCHEAP_NUM;t++){
		for(int i=0;i<MAX_SMALLSIZE/GRANULARITY+1;i++){
			procheaps[t][i].sc = &sizeclasses[i];
			FLUSH(&procheaps[t][i]);
		}
	}
	FLUSHFENCE;
}

uint64_t BaseMeta::new_space(int i){//i=0:desc, i=1:small sb, i=2:large sb
	if(space_num[i].load(std::memory_order_relaxed)>=MAX_SECTION) assert(0&&"space number reaches max!");
	FLUSHFENCE;
	uint64_t my_space_num = space_num[i].fetch_add(1);
	FLUSH(&space_num[i]);
	spaces[i][my_space_num].sec_bytes = 0;
	FLUSH(&spaces[i][my_space_num].sec_bytes);
	FLUSHFENCE;
	uint64_t space_size = i==0?DESC_SPACE_SIZE:SB_SPACE_SIZE;
	int res = mgr->__nvm_region_allocator(&(spaces[i][my_space_num].sec_start),PAGESIZE, space_size);
	if(res != 0) assert(0&&"region allocation fails!");
	// spaces[i][my_space_num].sec_curr.store(spaces[i][my_space_num].sec_start);
	spaces[i][my_space_num].sec_bytes = space_size;
	FLUSH(&spaces[i][my_space_num].sec_start);
	// FLUSH(&spaces[i][my_space_num].sec_curr);
	FLUSH(&spaces[i][my_space_num].sec_bytes);
	FLUSHFENCE;
	return my_space_num;
}


void* BaseMeta::small_sb_alloc(){
	void* sb = nullptr;
	int tid = get_thread_id();
	if(auto tmp = free_sb.dequeue(tid)){
		sb = tmp.value();
	}
	else{
		cout<<"allocate sb space "<<space_num<<endl;
		uint64_t space_num = new_space(1);
		sb = spaces[1][space_num].sec_start;
		organize_sb_list(sb,SB_SPACE_SIZE/SBSIZE,SBSIZE);
	}
	return sb;
}
void BaseMeta::small_sb_retire(void* sb){
	int tid = get_thread_id();
	free_sb.enqueue(sb,tid);
}
//todo
void* BaseMeta::large_sb_alloc(size_t size, uint64_t alignement){
	void* addr = mmap(nullptr,size,PROT_READ | PROT_WRITE, MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
	if (addr == MAP_FAILED) {
		fprintf(stderr, "large_sb_alloc() mmap failed, %lu: ", size);
		switch (errno) {
			case EBADF:	fprintf(stderr, "EBADF"); break;
			case EACCES:	fprintf(stderr, "EACCES"); break;
			case EINVAL:	fprintf(stderr, "EINVAL"); break;
			case ETXTBSY:	fprintf(stderr, "ETXBSY"); break;
			case EAGAIN:	fprintf(stderr, "EAGAIN"); break;
			case ENOMEM:	fprintf(stderr, "ENOMEM"); break;
			case ENODEV:	fprintf(stderr, "ENODEV"); break;
		}
		fprintf(stderr, "\n");
		fflush(stderr);
		exit(1);
	}
	else if(addr == nullptr){
		fprintf(stderr, "large_sb_alloc() mmap of size %lu returned NULL\n", size);
		fflush(stderr);
		exit(1);
	}
	return addr;
}
//todo
void BaseMeta::large_sb_retire(void* sb, size_t size){
	munmap(sb, size);
}
void BaseMeta::organize_desc_list(Descriptor* start, uint64_t count, uint64_t stride){
	// put new descs to free_desc queue
	uint64_t ptr = (uint64_t)start;
	int tid = get_thread_id();
	for(uint64_t i = 1; i < count; i++){
		ptr += stride;
		free_desc.enqueue((Descriptor*)ptr, tid);
	}

}
void BaseMeta::organize_sb_list(void* start, uint64_t count, uint64_t stride){
	// put new sbs to free_sb queue
	uint64_t ptr = (uint64_t)start;
	int tid = get_thread_id();
	for(uint64_t i = 1; i < count; i++){
		ptr += stride;
		free_sb.enqueue((void*)ptr, tid);
	}
}
void BaseMeta::organize_blk_list(void* start, uint64_t count, uint64_t stride){
//create linked freelist of blocks in the sb
	uint64_t ptr = (uint64_t)start; 
	for (uint64_t i = 1; i < count - 1; i++) {
		ptr += stride;
		*((uint64_t*)ptr) = i + 1;
	}
}


Descriptor* BaseMeta::desc_alloc(){
	Descriptor* desc = nullptr;
	int tid = get_thread_id();
	if(auto tmp = free_desc.dequeue(tid)){
		desc = tmp.value();
	}
	else {
		uint64_t space_num = new_space(0);
		// cout<<"allocate desc space "<<space_num<<endl;
		// spaces[0][space_num].sec_curr.store((void*)((size_t)spaces[0][space_num].sec_start+spaces[0][space_num].sec_bytes));
		desc = spaces[0][space_num].sec_start;
		organize_desc_list(desc, DESC_SPACE_SIZE/sizeof(Descriptor), sizeof(Descriptor));

		// desc = (Descriptor*)sb_alloc(DESCSBSIZE, sizeof(Descriptor));
		// organize_desc_list(desc, DESCSBSIZE/sizeof(Descriptor), sizeof(Descriptor));
	}
	return desc;
}
inline void BaseMeta::desc_retire(Descriptor* desc){
	int tid = get_thread_id();
	free_desc.enqueue(desc, tid);
}
Descriptor* BaseMeta::list_get_partial(Sizeclass* sc){
	//get a partial desc from sizeclass partial_desc queue
	int tid = get_thread_id();
	auto res = sc->partial_desc->dequeue(tid);
	if(res) return res.value();
	else return nullptr;
}
inline void BaseMeta::list_put_partial(Descriptor* desc){
	//put a partial desc to sizeclass partial_desc queue
	int tid = get_thread_id();
	desc->heap->sc->partial_desc->enqueue(desc,tid);
}
Descriptor* BaseMeta::heap_get_partial(Procheap* heap){
	Descriptor* desc = heap->partial.load();
	do{
		if(desc == nullptr){
			return list_get_partial(heap->sc);
		}
	}while(!heap->partial.compare_exchange_weak(desc,nullptr));
	return desc;
}
void BaseMeta::heap_put_partial(Descriptor* desc){
	//put desc to heap->partial
	Descriptor* prev = desc->heap->partial.load();
	while(!desc->heap->partial.compare_exchange_weak(prev,desc));
	if(prev){
		//put replaced partial desc to sizeclass partial desc queue
		list_put_partial(prev);
	}
}
void BaseMeta::list_remove_empty_desc(Sizeclass* sc){
	//try to retire empty descs from sc->partial_desc until reaches nonempty
	Descriptor* desc;
	int num_non_empty = 0;
	int tid = get_thread_id();
	while(auto tmp = sc->partial_desc->dequeue(tid)){
		desc = tmp.value();
		if(desc->sb == nullptr){
			desc_retire(desc);
		}
		else {
			sc->partial_desc->enqueue(desc,tid);
			if(++num_non_empty >= 2) break;
		}
	}
}
void BaseMeta::remove_empty_desc(Procheap* heap, Descriptor* desc){
	//remove the empty desc from heap->partial, or run list_remove_empty_desc on heap->sc
	if(heap->partial.compare_exchange_strong(desc,nullptr)) {
		desc_retire(desc);
	}
	else {
		list_remove_empty_desc(heap->sc);
	}
}
void BaseMeta::update_active(Procheap* heap, Descriptor* desc, uint64_t morecredits){
	Active oldactive, newactive;
	Anchor oldanchor, newanchor;
	
	*((uint64_t*)&oldactive) = 0;
	newactive.ptr = (uint64_t)desc>>6;
	newactive.credits = morecredits - 1;
	if(heap->active.compare_exchange_strong(oldactive,newactive)){
		//heap->active was NULL and is replaced by new one
		return;
	}
	oldanchor = desc->anchor.load();
	do{
		//return reserved morecredits to desc
		newanchor = oldanchor;
		newanchor.count += morecredits;
		newanchor.state = PARTIAL;
	}while(!desc->anchor.compare_exchange_weak(oldanchor,newanchor));
	//replace heap->partial by desc
	heap_put_partial(desc);
}
inline Descriptor* BaseMeta::mask_credits(Active oldactive){
	uint64_t ret = oldactive.ptr;
	return (Descriptor*)(ret<<6);
}


Procheap* BaseMeta::find_heap(size_t sz){
	// We need to fit both the object and the descriptor in a single block
	sz += HEADER_SIZE;
	if (sz > 2048) {
		return nullptr;
	}
	int tid = get_thread_id();
	return &procheaps[tid][sz / GRANULARITY];
}
void* BaseMeta::malloc_from_active(Procheap* heap){
	Active newactive, oldactive;
	oldactive = heap->active.load();
	void* addr = nullptr;
	//1. deduce credit if non-zero
	do{
		newactive = oldactive;
		if(!(*((uint64_t*)(&oldactive)))){//oldactive is NULL
			return nullptr;
		}
		if(oldactive.credits == 0){
			//only one reserved block left, set heao->active to NULL
			*((uint64_t*)(&newactive)) = 0;
		}
		else {
			--newactive.credits;
		}
	} while(!heap->active.compare_exchange_weak(oldactive,newactive));
	//2. get the block of corresponding credit
	Descriptor* desc = mask_credits(oldactive);
	Anchor oldanchor,newanchor;
	oldanchor = desc->anchor.load();
	uint64_t morecredits = 0;
	do{
		newanchor = oldanchor;
		addr = (void*) ((uint64_t)desc->sb + oldanchor.avail * desc->sz);
		uint64_t next = *(uint64_t*)addr;
		newanchor.avail = next;
		newanchor.tag++;
		if(oldactive.credits == 0){
			//this is the last reserved block in oldactive
			if(oldanchor.count == 0){
				newanchor.state = FULL;
			}
			else{
				//reserve more for active desc
				morecredits = min(oldanchor.count,MAXCREDITS);
				newanchor.count -= morecredits;
			}
		}
	} while(!desc->anchor.compare_exchange_weak(oldanchor,newanchor));
	if(oldactive.credits == 0 && oldanchor.count > 0){
		//old active desc runs out but the desc has more credits to reserve
		update_active(heap,desc,morecredits);
	}
	*((char*)addr) = (char)SMALL;
	addr += TYPE_SIZE;
	*((Descriptor**)addr) = desc;
	FLUSH(addr - TYPE_SIZE);
	FLUSH(addr);
	FLUSHFENCE;
	return ((void*)((uint64_t)addr + PTR_SIZE));
}
void* BaseMeta::malloc_from_partial(Procheap* heap){
	Descriptor* desc = nullptr;
	Anchor oldanchor,newanchor;
	uint64_t morecredits = 0;
	void* addr;

retry:
	//grab the partial desc from heap or sizeclass (exclusively)
	desc = heap_get_partial(heap);
	if(!desc){
		return nullptr;
	}
	desc->heap = heap;
	FLUSH(&desc->heap);
	oldanchor = desc->anchor.load();
	do{
		//reserve blocks
		newanchor = oldanchor;
		if(oldanchor.state == EMPTY){
			desc_retire(desc);
			goto retry;
		}
		//oldanchor state must be PARTIAL, and count must > 0
		morecredits = min(oldanchor.count - 1, MAXCREDITS);
		newanchor.count -= morecredits + 1;
		newanchor.state = (morecredits>0)?ACTIVE:FULL;
	}while(!desc->anchor.compare_exchange_weak(oldanchor,newanchor));

	oldanchor = desc->anchor.load();
	do{
		//pop reserved block
		newanchor = oldanchor;
		addr = (void*)((uint64_t)desc->sb + oldanchor.avail * desc->sz);
		newanchor.avail = *(uint64_t*)addr;
		newanchor.tag++;
	}while(!desc->anchor.compare_exchange_weak(oldanchor,newanchor));

	if(morecredits > 0){
		update_active(heap, desc, morecredits);
	}
	*((char*)addr) = (char)SMALL;
	addr += TYPE_SIZE;
	*((Descriptor**)addr) = desc;
	FLUSH(addr-TYPE_SIZE);
	FLUSH(addr);
	FLUSHFENCE;
	return ((void *)((uint64_t)addr + PTR_SIZE));

}
void* BaseMeta::malloc_from_newsb(Procheap* heap){
	Active oldactive,newactive;
	Anchor newanchor;
	void* addr = nullptr;

	Descriptor* desc = desc_alloc();
	desc->sb = small_sb_alloc();
	desc->heap = heap;
	newanchor.avail = 1;
	desc->sz = heap->sc->sz;
	desc->maxcount = heap->sc->sbsize / desc->sz;
	FLUSH(&desc->sb);
	FLUSH(&desc->heap);
	FLUSH(&desc->sz);
	FLUSH(&desc->maxcount);
	organize_blk_list(desc->sb, desc->maxcount, desc->sz);

	newactive.ptr = (uint64_t)desc>>6;
	newactive.credits = min(desc->maxcount - 1, MAXCREDITS)-1;
	newanchor.count = max(((int)desc->maxcount - 1)-((int)newactive.credits + 1), 0);
	newanchor.state = ACTIVE;
	desc->anchor.store(newanchor);

	*((uint64_t*)(&oldactive)) = 0;
	if(heap->active.compare_exchange_strong(oldactive,newactive)){
		addr = desc->sb;
		*((char*)addr) = (char)SMALL;
		addr += TYPE_SIZE;
		*((Descriptor**)addr) = desc;
		FLUSH(addr-TYPE_SIZE);
		FLUSH(addr);
		FLUSHFENCE;
		return (void*)((uint64_t)addr + PTR_SIZE);
	}
	else {
		small_sb_retire(desc->sb);
		desc_retire(desc);
		return nullptr;
	}
}
void* BaseMeta::alloc_large_block(size_t sz){
	void* addr = large_sb_alloc(sz + HEADER_SIZE, SBSIZE);
	*((char*)addr) = (char)LARGE;
	addr += TYPE_SIZE;
	*((uint64_t*)addr) = sz + HEADER_SIZE;
	FLUSH(addr-TYPE_SIZE);
	FLUSH(addr);
	FLUSHFENCE;
	return (void*)(addr + PTR_SIZE);
}


