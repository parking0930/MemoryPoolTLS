#pragma once
#include "MemoryPool.h"
#define NUM_OF_BLOCK_IN_CHUNK	400
#define POOL_MAX_ALLOC			0xFFFFFFFFFFFFFFF
//#define MEMORYPOOL_MODE_RELEASE
// 메인풀에서 Chunk의 할당과 반환에만 경합이 생김

template <class T, size_t ChunkSize = NUM_OF_BLOCK_IN_CHUNK>
class MemoryPoolTLS
{
private:
	struct BLOCK
	{
#ifndef MEMORYPOOL_MODE_RELEASE
		code_t	_preCode;
#endif // !MEMORYPOOL_MODE_RELEASE
		T		_data;
		BLOCK*	_next;
#ifndef MEMORYPOOL_MODE_RELEASE
		code_t	_postCode;
#endif // !MEMORYPOOL_MODE_RELEASE
	};

	// 블록들을 ChunkSize개까지 담을 수 있는 클래스
	class Chunk
	{
	public:
		Chunk();
		~Chunk() {}
		bool Push(BLOCK* pBlock);
		bool Pop(BLOCK** pDestBlock);
	private:
		bool isEmpty() { return _size == 0; }
		bool isFull() { return _size == ChunkSize; }
	public:
		volatile Chunk* _next;
		BLOCK*			_freeStackTop;
	private:
		UINT64			_size;
	};

	// 확장한 블록들을 관리하기 위한 클래스
	class BlockArrayAllocator
	{
	private:
		struct NODE
		{
			BLOCK	_blockPool[ChunkSize];
			NODE*	_next;
		};
	public:
		BlockArrayAllocator();
		~BlockArrayAllocator();
		BLOCK* Alloc();
	private:
		void Push(NODE* pPushNode);
		bool Pop(NODE** pPopedNode);
	private:
		volatile NODE*	_allocTop;
	};

	////////////////////////////////////////////////////////////////////////////////////////////////////
public:
	MemoryPoolTLS() = delete;
	template <typename... Types>
	MemoryPoolTLS(UINT64 initBlockNum, UINT64 maxBlockLimit, bool isPlacementNew, Types... args);
	~MemoryPoolTLS();
	template <typename... Types>
	T* Alloc(Types... args);
	bool Free(T* data);
private:
	template <typename... Types>
	Chunk* ChunkAlloc(Types... args); // Freelist 관리까지 담당
	void ChunkFree(Chunk* pChunk);
	// Chunk FreeStack
	void Push(Chunk* pChunk);
	bool Pop(Chunk** pDestChunk);
private:
	// Block Pool
	BLOCK*	_blockPool;

	// Etc
	void*	_allocAddr;

	// Additional chunk allocator, ChunkSize개의 블록이 담긴 배열 추가 생성
	BlockArrayAllocator*	_addBlockArrAllocator;
	UINT64					_addAllocCnt;
	UINT64					_addAllocMax;

	// ChunkManager, 빈 Chunk 할당과 반환 담당
	MemoryPool<Chunk>*	_emptyChunkManager;

	// Chunk FreeStack, Block으로 가득찬 소유자 없는 Chunk 보관
	volatile Chunk* _freeStackTop;

	// Setting
	const bool		_isPlacementNew;

	//	COMMON
	DWORD			_tlsIdx;

#ifndef MEMORYPOOL_MODE_RELEASE
	// Debugging
	static code_t	secure_code;
	UINT64			_totalChunk;
	UINT64			_useChunk;
	code_t			_myCode;
#endif // !MEMORYPOOL_MODE_RELEASE
};

///////////////////////////////////////////////////////////////////////////////////////
#ifndef MEMORYPOOL_MODE_RELEASE
template <class T, size_t ChunkSize>
code_t MemoryPoolTLS<T, ChunkSize>::secure_code = 0;
#endif // !MEMORYPOOL_MODE_RELEASE

template <class T, size_t ChunkSize>
template <typename... Types>
MemoryPoolTLS<T, ChunkSize>::MemoryPoolTLS(UINT64 initBlockNum, UINT64 maxBlockLimit, bool isPlacementNew, Types... args)
	:_isPlacementNew(isPlacementNew), _freeStackTop(nullptr), _addAllocCnt(-1), _allocAddr(nullptr)
{
	SYSTEM_INFO si;
	GetSystemInfo(&si);
	if ((__int64)si.lpMaximumApplicationAddress != 0x00007FFFFFFEFFFF)
		throw L"It does not work with the current version of the current OS.";

	if (initBlockNum > maxBlockLimit)
		throw L"Error: initBlockNum is larger than maxBlockLimit.";

	if (initBlockNum == 0 && maxBlockLimit == 0)
		throw L"Error: Parameter is not correct.";

	_tlsIdx = TlsAlloc();

	UINT64 initChunkCnt = (initBlockNum + (ChunkSize - 1)) / ChunkSize;
	UINT64 maxChunkCnt = (maxBlockLimit + (ChunkSize - 1)) / ChunkSize;
	maxBlockLimit = maxChunkCnt * ChunkSize;

#ifndef MEMORYPOOL_MODE_RELEASE
	_totalChunk = initChunkCnt;
	_useChunk = 0;
	_myCode = InterlockedIncrement64((PLONG64)&secure_code);
#endif // !MEMORYPOOL_MODE_RELEASE

	_addAllocMax = maxChunkCnt - initChunkCnt;
	if (_addAllocMax > 0)
		_addBlockArrAllocator = new BlockArrayAllocator();
	else
		_addBlockArrAllocator = nullptr;

	// 빈 Chunk 할당할 메모리풀 생성
	_emptyChunkManager = new MemoryPool<Chunk>(initChunkCnt + si.dwNumberOfProcessors, POOL_MAX_ALLOC, false);

	// BlockPool 생성
	if (initBlockNum == 0)	// 처음부터 FreeList 형태로 갈 경우
	{
		_blockPool = nullptr;
	}
	else
	{
		size_t allSize = initChunkCnt * ChunkSize * sizeof(BLOCK) + alignof(BLOCK);
		size_t allocCnt = (allSize + (si.dwAllocationGranularity - 1)) / si.dwAllocationGranularity;
		size_t allocByte = si.dwAllocationGranularity * allocCnt;
		_allocAddr = VirtualAlloc(NULL, allocByte, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
		if (_allocAddr == nullptr)
			throw GetLastError();
		size_t addr = reinterpret_cast<size_t>(_allocAddr);
		size_t alignCnt = (addr + alignof(BLOCK) - 1) / alignof(BLOCK);
		_blockPool = reinterpret_cast<BLOCK*>(alignof(BLOCK) * alignCnt);


		Chunk* pChunk;
		for (size_t i = 0; i < initChunkCnt; i++)
		{
			pChunk = _emptyChunkManager->Alloc();
			size_t edIdx = ChunkSize * (i + 1);
			for (size_t j = ChunkSize * i; j < edIdx; j++)
			{
#ifndef MEMORYPOOL_MODE_RELEASE
				_blockPool[j]._preCode = _myCode;
				_blockPool[j]._postCode = ~_myCode;
#endif // !MEMORYPOOL_MODE_RELEASE
				pChunk->Push(&_blockPool[j]);
				if (!isPlacementNew)
					new (&_blockPool[j]._data) T(args...);
			}

			pChunk->_next = _freeStackTop;
			_freeStackTop = pChunk;
		}
	}

}

template <class T, size_t ChunkSize>
MemoryPoolTLS<T, ChunkSize>::~MemoryPoolTLS()
{
	if (_allocAddr != nullptr)
		VirtualFree(_allocAddr, 0, MEM_RELEASE);

	if (_addBlockArrAllocator != nullptr)
		delete _addBlockArrAllocator;

	delete		_emptyChunkManager;
	TlsFree(_tlsIdx);
}

template <class T, size_t ChunkSize>
template <typename... Types>
T* MemoryPoolTLS<T, ChunkSize>::Alloc(Types... args)
{
	BLOCK* pPopedBlock;

	Chunk* pTlsChunk = reinterpret_cast<Chunk*>(TlsGetValue(_tlsIdx));
	if (pTlsChunk == nullptr)
	{
		pTlsChunk = ChunkAlloc(args...);
		TlsSetValue(_tlsIdx, pTlsChunk);
	}

	// Chunk에서 Block 할당
	if (!pTlsChunk->Pop(&pPopedBlock))
	{
		// 빈 껍데기 Chunk 반환
		_emptyChunkManager->Free(pTlsChunk);
		// 새 Chunk 할당
		pTlsChunk = ChunkAlloc(args...);
		pTlsChunk->Pop(&pPopedBlock);
		TlsSetValue(_tlsIdx, pTlsChunk);
	}

#ifndef MEMORYPOOL_MODE_RELEASE
	// 할당 메모리 구분용 코드로 변환
	pPopedBlock->_postCode = ~pPopedBlock->_postCode;
#endif // !MEMORYPOOL_MODE_RELEASE

	if (_isPlacementNew)
		return new (&pPopedBlock->_data) T(args...);
	else
		return &pPopedBlock->_data;
}

template <class T, size_t ChunkSize>
bool MemoryPoolTLS<T, ChunkSize>::Free(T* data)
{
#ifndef MEMORYPOOL_MODE_RELEASE
	BLOCK* pushNode = reinterpret_cast<BLOCK*>(
		reinterpret_cast<__int64>(data) - reinterpret_cast<__int64>(&((BLOCK*)0)->_data)
		);

	// 메모리 침범 및 유효성 검사(할당받지 않은 메모리인지 파악)
	if (pushNode->_preCode != pushNode->_postCode)
		return false;

	// 다른 메모리풀에 반환하려하는 경우인지 검사
	if (pushNode->_preCode != _myCode)
		return false;

	// 해제 메모리 구분용 코드로 변환
	pushNode->_postCode = ~pushNode->_postCode;
#else
	BLOCK* pushNode = reinterpret_cast<BLOCK*>(data);
#endif // !MEMORYPOOL_MODE_RELEASE

	// 소멸자 호출 여부 결정
	if (_isPlacementNew)
		data->~T();

	Chunk* pTlsChunk = reinterpret_cast<Chunk*>(TlsGetValue(_tlsIdx));
	if (pTlsChunk == nullptr)
	{
		// 빈 껍데기 Chunk 할당
		pTlsChunk = _emptyChunkManager->Alloc();
		TlsSetValue(_tlsIdx, pTlsChunk);
	}

	if(!pTlsChunk->Push(pushNode))
	{
		ChunkFree(pTlsChunk);
		pTlsChunk = _emptyChunkManager->Alloc();
		pTlsChunk->Push(pushNode);
		TlsSetValue(_tlsIdx, pTlsChunk);
	}
	return true;
}


template <class T, size_t ChunkSize>
template <typename... Types>
typename MemoryPoolTLS<T, ChunkSize>::Chunk* MemoryPoolTLS<T, ChunkSize>::ChunkAlloc(Types... args)
{
	Chunk* popChunk = nullptr;
	if (!Pop(&popChunk))
	{
		UINT64 myNum = InterlockedIncrement64((PLONG64)&_addAllocCnt);
		if (myNum == _addAllocMax)
		{
			return nullptr;
		}
		else if (myNum > _addAllocMax)
		{
			InterlockedDecrement64((PLONG64)&_addAllocCnt);
			return nullptr;
		}

		// Additional allocator에서 추가 블록 배열(ChunkSize개) 할당받음
		BLOCK* pBlockArr = _addBlockArrAllocator->Alloc();

		// 할당에 실패한 경우
		if (pBlockArr == nullptr)
			return nullptr;

		// 빈 Chunk 할당
		popChunk = _emptyChunkManager->Alloc();

		for (int i = 0; i < ChunkSize; i++)
		{
#ifndef MEMORYPOOL_MODE_RELEASE
			pBlockArr[i]._preCode = _myCode;
			pBlockArr[i]._postCode = ~_myCode;
#endif // !MEMORYPOOL_MODE_RELEASE
			popChunk->Push(&pBlockArr[i]);
			if (!_isPlacementNew)
				new (&pBlockArr[i]._data) T(args...);
		}

#ifndef MEMORYPOOL_MODE_RELEASE
		InterlockedIncrement64((PLONG64)&_totalChunk);
#endif // !MEMORYPOOL_MODE_RELEASE
	}
#ifndef MEMORYPOOL_MODE_RELEASE
	InterlockedIncrement64((PLONG64)&_useChunk);
#endif // !MEMORYPOOL_MODE_RELEASE

	return popChunk;
}

template <class T, size_t ChunkSize>
void MemoryPoolTLS<T, ChunkSize>::ChunkFree(Chunk* pChunk)
{
	Push(pChunk);
#ifndef MEMORYPOOL_MODE_RELEASE
	InterlockedDecrement64((PLONG64)&_useChunk);
#endif // !MEMORYPOOL_MODE_RELEASE
}

template <class T, size_t ChunkSize>
void MemoryPoolTLS<T, ChunkSize>::Push(Chunk* pChunk)
{
	volatile UINT64 idx;
	volatile Chunk* bkTop;
	while (1)
	{
		bkTop = _freeStackTop;
		idx = (UINT64)bkTop >> 47;
		pChunk->_next = (Chunk*)((UINT64)bkTop & 0x00007FFFFFFFFFFF);
		if (InterlockedCompareExchange64((PLONG64)&_freeStackTop, (LONG64)pChunk | ((UINT64)idx << 47), (LONG64)bkTop) == (LONG64)bkTop)
			break;
	}
}

template <class T, size_t ChunkSize>
bool MemoryPoolTLS<T, ChunkSize>::Pop(Chunk** pDestChunk)
{
	volatile void* bkTop;
	volatile UINT64 idx;
	Chunk* popNode;
	volatile Chunk* nextTop;
	while (1)
	{
		bkTop = _freeStackTop;
		idx = ((UINT64)bkTop >> 47) + 1;
		popNode = (Chunk*)((UINT64)bkTop & 0x00007FFFFFFFFFFF);
		if (popNode == nullptr)
			return false;

		nextTop = popNode->_next;

		if (InterlockedCompareExchange64((PLONG64)&_freeStackTop, (LONG64)nextTop | ((UINT64)idx << 47), (LONG64)bkTop) == (LONG64)bkTop)
			break;
	}
	*pDestChunk = popNode;

	return true;
}
///////////////////////////////////////////////////////////////////////////////////////
/* Chunk Class */
template <class T, size_t ChunkSize>
MemoryPoolTLS<T, ChunkSize>::Chunk::Chunk() :_size(0), _freeStackTop(nullptr), _next(nullptr) {}

template <class T, size_t ChunkSize>
bool MemoryPoolTLS<T, ChunkSize>::Chunk::Push(BLOCK* pBlock)
{
	if (isFull())
		return false;
	pBlock->_next = _freeStackTop;
	_freeStackTop = pBlock;
	++_size;
	return true;
}

template <class T, size_t ChunkSize>
bool MemoryPoolTLS<T, ChunkSize>::Chunk::Pop(BLOCK** pDestBlock)
{
	if (isEmpty())
		return false;
	*pDestBlock = _freeStackTop;
	_freeStackTop = _freeStackTop->_next;
	--_size;
	return true;
}
///////////////////////////////////////////////////////////////////////////////////////
/* ChunkAllocator class */
template <class T, size_t ChunkSize>
MemoryPoolTLS<T, ChunkSize>::BlockArrayAllocator::BlockArrayAllocator() :_allocTop(nullptr) {}

template <class T, size_t ChunkSize>
MemoryPoolTLS<T, ChunkSize>::BlockArrayAllocator::~BlockArrayAllocator()
{
	NODE* popNode;
	while (Pop(&popNode))
		_aligned_free(popNode);
}

template <class T, size_t ChunkSize>
typename MemoryPoolTLS<T, ChunkSize>::BLOCK* MemoryPoolTLS<T, ChunkSize>::BlockArrayAllocator::Alloc()
{
	NODE* newNode = reinterpret_cast<NODE*>(_aligned_malloc(sizeof(NODE), alignof(NODE)));
	if (newNode == nullptr)
		return nullptr;

	Push(newNode);
	return newNode->_blockPool;
}

template <class T, size_t ChunkSize>
void  MemoryPoolTLS<T, ChunkSize>::BlockArrayAllocator::Push(NODE* pPushNode)
{
	volatile UINT64 idx;
	volatile NODE* bkTop;
	while (1)
	{
		bkTop = _allocTop;
		idx = (UINT64)bkTop >> 47;
		pPushNode->_next = (NODE*)((UINT64)bkTop & 0x00007FFFFFFFFFFF);
		if (InterlockedCompareExchange64((PLONG64)&_allocTop, (LONG64)pPushNode | ((UINT64)idx << 47), (LONG64)bkTop) == (LONG64)bkTop)
			break;
	}
}

template <class T, size_t ChunkSize>
bool  MemoryPoolTLS<T, ChunkSize>::BlockArrayAllocator::Pop(NODE** pPopedNode)
{
	volatile void* bkTop;
	volatile UINT64 idx;
	NODE* popNode;
	volatile NODE* nextTop;
	while (1)
	{
		bkTop = _allocTop;
		idx = ((UINT64)bkTop >> 47) + 1;
		popNode = (NODE*)((UINT64)bkTop & 0x00007FFFFFFFFFFF);
		if (popNode == nullptr)
			return false;

		nextTop = popNode->_next;

		if (InterlockedCompareExchange64((PLONG64)&_allocTop, (LONG64)nextTop | ((UINT64)idx << 47), (LONG64)bkTop) == (LONG64)bkTop)
			break;
	}
	*pPopedNode = popNode;

	return true;
}
