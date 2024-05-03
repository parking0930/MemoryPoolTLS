#pragma once
#include <Windows.h>
#define POOL_MAX_ALLOC			0xFFFFFFFFFFFFFFF
#define code_t	unsigned __int64

/*
# LockFree MemoryPool(ObjectPool)

이 프로젝트의 메모리풀은 64bit 빌드로만 동작하며
기본적으로 Thread safe하게 구현되어있습니다.(LockFree)

디버깅 목적을 달성하였고 성능상의 이득을 위한다면 Release 모드를 사용해야 합니다.
이 메모리풀에서의 릴리즈 모드란 컴파일러의 Release 모드가 아닌
아래와 같이 MEMORYPOOL_MODE_RELEASE을 define 하는 것을 의미합니다.

#define MEMORYPOOL_MODE_RELEASE

만약 MEMORYPOOL_MODE_RELEASE를 define하지 않는다면 이 메모리풀은
기본적으로 Debug 모드로 동작하게 됩니다.

Release 모드는 Debug 모드에서의 잘못된 메모리풀에 반환하는 것과
댕글링 포인터 반환, 할당되지 않은 메모리 반환을 감지하지 못합니다.

# 박재현(parking0930@naver.com)
*/

template <class T>
class MemoryPool
{
public:
	struct BLOCK
	{
#ifndef MEMORYPOOL_MODE_RELEASE
		code_t	_preCode;
#endif // !MEMORYPOOL_MODE_RELEASE
		T		_data;
		volatile BLOCK* _next;
#ifndef MEMORYPOOL_MODE_RELEASE
		code_t		_postCode;
#endif // !MEMORYPOOL_MODE_RELEASE
	};
	class BlockAllocator
	{
	private:
		struct NODE
		{
			BLOCK	_block;
			NODE* _next;
		};
	public:
		BlockAllocator();
		~BlockAllocator();
		BLOCK* Alloc();
	private:
		void Push(NODE* pPushNode);
		bool Pop(NODE** pPopedNode);
	private:
		volatile NODE* _allocTop;
	};
	MemoryPool() = delete;
	template <typename... Types>
	MemoryPool(UINT64 initBlockNum, UINT64 maxBlockLimit, bool isPlacementNew, Types... args);
	~MemoryPool();
	template <typename... Types>
	T* Alloc(Types... args);
	bool Free(T* data);

private:
	BLOCK* _blockPool;

	// MemoryPool FreeStack //
	volatile BLOCK* _freeStackTop;
	void Push(BLOCK* data);
	bool Pop(BLOCK** data);

	// Additional block allocator //
	BlockAllocator* _addBlockAllocator;
	UINT64				_addAllocCnt;
	UINT64				_addAllocMax;

	// Etc //
	void* _allocAddr;

	/////// Debugging ///////
#ifndef MEMORYPOOL_MODE_RELEASE
	static code_t secure_code;
	UINT64	_useCnt;
	UINT64	_totalCnt;
	code_t	_myCode;
#endif // !MEMORYPOOL_MODE_RELEASE

	// Setting
	const bool	_isPlacementNew;
};

#ifndef MEMORYPOOL_MODE_RELEASE
template <class T>
code_t MemoryPool<T>::secure_code = 0;
#endif // !MEMORYPOOL_MODE_RELEASE

template <class T>
template <typename... Types>
MemoryPool<T>::MemoryPool(UINT64 initBlockNum, UINT64 maxBlockLimit, bool isPlacementNew, Types... args)
	:_isPlacementNew(isPlacementNew), _addAllocMax(0), _addAllocCnt(-1), _allocAddr(nullptr)
{
	SYSTEM_INFO si;
	GetSystemInfo(&si);
	if ((__int64)si.lpMaximumApplicationAddress != 0x00007FFFFFFEFFFF)
		throw L"It does not work with the current version of the current OS.";

	if (initBlockNum > maxBlockLimit)
		throw L"Error: initBlockNum is larger than maxBlockLimit.";

	if (initBlockNum == 0 && maxBlockLimit == 0)
		throw L"Error: Parameter is not correct.";

#ifndef MEMORYPOOL_MODE_RELEASE
	_myCode = InterlockedIncrement64((PLONG64)&secure_code);
#endif // !MEMORYPOOL_MODE_RELEASE

	// FreeList로 확장 여부 결정
	if (maxBlockLimit - initBlockNum > 0)
	{
		_addBlockAllocator = new BlockAllocator();
		_addAllocMax = maxBlockLimit - initBlockNum;
	}
	else
	{
		_addBlockAllocator = nullptr;
	}

	_freeStackTop = nullptr;
	if (initBlockNum == 0)	// 처음부터 FreeList 형태로 갈 경우
	{
		_blockPool = nullptr;
	}
	else				// Memory Pool로 시작하는 경우
	{
		size_t allSize = sizeof(BLOCK) * initBlockNum + alignof(BLOCK);
		size_t allocCnt = (allSize + (si.dwAllocationGranularity - 1)) / si.dwAllocationGranularity;
		size_t allocByte = si.dwAllocationGranularity * allocCnt;
		_allocAddr = VirtualAlloc(NULL, allocByte, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
		if (_allocAddr == nullptr)
			throw GetLastError();
		size_t addr = reinterpret_cast<size_t>(_allocAddr);
		size_t alignCnt = (addr + alignof(BLOCK) - 1) / alignof(BLOCK);
		_blockPool = reinterpret_cast<BLOCK*>(alignof(BLOCK) * alignCnt);

		for (int i = 0; i < initBlockNum; i++)
		{
#ifndef MEMORYPOOL_MODE_RELEASE
			_blockPool[i]._preCode = _myCode;
			_blockPool[i]._postCode = ~_myCode;
#endif // !MEMORYPOOL_MODE_RELEASE
			_blockPool[i]._next = _freeStackTop;
			_freeStackTop = &_blockPool[i];
			if (!isPlacementNew)
				new (&_blockPool[i]._data) T(args...);
		}
	}

#ifndef MEMORYPOOL_MODE_RELEASE
	// Debug
	_useCnt = 0;
	_totalCnt = initBlockNum;
#endif // !MEMORYPOOL_MODE_RELEASE
}

template <class T>
MemoryPool<T>::~MemoryPool()
{
	if (_allocAddr != nullptr)
		VirtualFree(_allocAddr, 0, MEM_RELEASE);

	if (_addBlockAllocator != nullptr)
		delete _addBlockAllocator;
}

template <class T>
template <typename... Types>
T* MemoryPool<T>::Alloc(Types... args)
{
	BLOCK* popNode = nullptr;

	if (!Pop(&popNode)) // 확장 할당이 필요한 경우
	{
		// 추가로 확장해서 할당할 수 블록이 없는 경우
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

		// Additional allocator에서 추가 블록 할당받음
		popNode = _addBlockAllocator->Alloc();

		// 할당에 실패한 경우
		if (popNode == nullptr)
			return nullptr;

#ifndef MEMORYPOOL_MODE_RELEASE
		popNode->_preCode = _myCode;
		popNode->_postCode = _myCode; // 할당 메모리 구분 코드
		InterlockedIncrement64((PLONG64)&_totalCnt);
#endif // !MEMORYPOOL_MODE_RELEASE

		popNode->_next = nullptr;
		if (!_isPlacementNew)
			new (&popNode->_data) T(args...);
	}
#ifndef MEMORYPOOL_MODE_RELEASE
	else // 확장 할당이 필요 없는 경우
	{
		// 할당받은 블록의 postCode를 할당 메모리 구분용 코드로 변환
		popNode->_postCode = ~(popNode->_postCode);
	}
#endif // !MEMORYPOOL_MODE_RELEASE

#ifndef MEMORYPOOL_MODE_RELEASE
	InterlockedIncrement64((PLONG64)&_useCnt);
#endif // !MEMORYPOOL_MODE_RELEASE

	if (_isPlacementNew)
		return new (&popNode->_data) T(args...);
	else
		return &popNode->_data;
}

template <class T>
bool MemoryPool<T>::Free(T* data)
{
#ifndef MEMORYPOOL_MODE_RELEASE
	BLOCK* pushNode = reinterpret_cast<BLOCK*>(
		reinterpret_cast<UINT64>(data) - reinterpret_cast<UINT64>(&((BLOCK*)0)->_data)
		);

	// 메모리 침범 및 유효성 검사(할당받지 않은 메모리인지 파악)
	if (pushNode->_preCode != pushNode->_postCode)
		return false;

	// 다른 메모리풀에 반환하려하는 경우인지 검사
	if (pushNode->_preCode != _myCode)
		return false;

	// 해제 메모리 구분용 코드로 변환
	pushNode->_postCode = ~(pushNode->_postCode);

	InterlockedDecrement64((PLONG64)&_useCnt);
#else
	BLOCK* pushNode = reinterpret_cast<BLOCK*>(data);
#endif // !MEMORYPOOL_MODE_RELEASE

	// 소멸자 호출 여부 결정
	if (_isPlacementNew)
		data->~T();

	// 메모리풀로 반환된 블록들을 보관하는 스택에 PUSH
	Push(pushNode);
	return true;
}

template <class T>
void MemoryPool<T>::Push(BLOCK* data)
{
	UINT64 idx;
	volatile BLOCK* bkTop;
	while (1)
	{
		bkTop = _freeStackTop;
		idx = (UINT64)bkTop >> 47;
		data->_next = (BLOCK*)((UINT64)bkTop & 0x00007FFFFFFFFFFF);
		if (InterlockedCompareExchange64((PLONG64)&_freeStackTop, (LONG64)data | ((UINT64)idx << 47), (LONG64)bkTop) == (LONG64)bkTop)
			break;
	}
}

template <class T>
bool MemoryPool<T>::Pop(BLOCK** data)
{
	volatile void* bkTop;
	UINT64 idx;
	BLOCK* popNode;
	volatile BLOCK* nextTop;
	while (1)
	{
		bkTop = _freeStackTop;
		idx = ((UINT64)bkTop >> 47) + 1;
		popNode = (BLOCK*)((UINT64)bkTop & 0x00007FFFFFFFFFFF);
		if (popNode == nullptr)
			return false;

		nextTop = popNode->_next;

		if (InterlockedCompareExchange64((PLONG64)&_freeStackTop, (LONG64)nextTop | ((UINT64)idx << 47), (LONG64)bkTop) == (LONG64)bkTop)
			break;
	}
	*data = popNode;

	return true;
}
///////////////////////////////////////////////////////////////////////////////////////////
/* BlockAllocator class */
template <class T>
MemoryPool<T>::BlockAllocator::BlockAllocator() :_allocTop(nullptr) {}

template <class T>
MemoryPool<T>::BlockAllocator::~BlockAllocator()
{
	NODE* popNode;
	while (Pop(&popNode))
		_aligned_free(popNode);
}

template <class T>
typename MemoryPool<T>::BLOCK* MemoryPool<T>::BlockAllocator::Alloc()
{
	NODE* newNode = reinterpret_cast<NODE*>(_aligned_malloc(sizeof(NODE), alignof(NODE)));
	if (newNode == nullptr)
		return nullptr;

	Push(newNode);
	return &newNode->_block;
}

template <class T>
void  MemoryPool<T>::BlockAllocator::Push(NODE* pPushNode)
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

template <class T>
bool  MemoryPool<T>::BlockAllocator::Pop(NODE** pPopedNode)
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
