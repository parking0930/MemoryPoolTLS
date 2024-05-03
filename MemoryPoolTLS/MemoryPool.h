#pragma once
#include <Windows.h>
#define POOL_MAX_ALLOC			0xFFFFFFFFFFFFFFF
#define code_t	unsigned __int64

/*
# LockFree MemoryPool(ObjectPool)

�� ������Ʈ�� �޸�Ǯ�� 64bit ����θ� �����ϸ�
�⺻������ Thread safe�ϰ� �����Ǿ��ֽ��ϴ�.(LockFree)

����� ������ �޼��Ͽ��� ���ɻ��� �̵��� ���Ѵٸ� Release ��带 ����ؾ� �մϴ�.
�� �޸�Ǯ������ ������ ���� �����Ϸ��� Release ��尡 �ƴ�
�Ʒ��� ���� MEMORYPOOL_MODE_RELEASE�� define �ϴ� ���� �ǹ��մϴ�.

#define MEMORYPOOL_MODE_RELEASE

���� MEMORYPOOL_MODE_RELEASE�� define���� �ʴ´ٸ� �� �޸�Ǯ��
�⺻������ Debug ���� �����ϰ� �˴ϴ�.

Release ���� Debug ��忡���� �߸��� �޸�Ǯ�� ��ȯ�ϴ� �Ͱ�
��۸� ������ ��ȯ, �Ҵ���� ���� �޸� ��ȯ�� �������� ���մϴ�.

# ������(parking0930@naver.com)
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

	// FreeList�� Ȯ�� ���� ����
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
	if (initBlockNum == 0)	// ó������ FreeList ���·� �� ���
	{
		_blockPool = nullptr;
	}
	else				// Memory Pool�� �����ϴ� ���
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

	if (!Pop(&popNode)) // Ȯ�� �Ҵ��� �ʿ��� ���
	{
		// �߰��� Ȯ���ؼ� �Ҵ��� �� ����� ���� ���
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

		// Additional allocator���� �߰� ��� �Ҵ����
		popNode = _addBlockAllocator->Alloc();

		// �Ҵ翡 ������ ���
		if (popNode == nullptr)
			return nullptr;

#ifndef MEMORYPOOL_MODE_RELEASE
		popNode->_preCode = _myCode;
		popNode->_postCode = _myCode; // �Ҵ� �޸� ���� �ڵ�
		InterlockedIncrement64((PLONG64)&_totalCnt);
#endif // !MEMORYPOOL_MODE_RELEASE

		popNode->_next = nullptr;
		if (!_isPlacementNew)
			new (&popNode->_data) T(args...);
	}
#ifndef MEMORYPOOL_MODE_RELEASE
	else // Ȯ�� �Ҵ��� �ʿ� ���� ���
	{
		// �Ҵ���� ����� postCode�� �Ҵ� �޸� ���п� �ڵ�� ��ȯ
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

	// �޸� ħ�� �� ��ȿ�� �˻�(�Ҵ���� ���� �޸����� �ľ�)
	if (pushNode->_preCode != pushNode->_postCode)
		return false;

	// �ٸ� �޸�Ǯ�� ��ȯ�Ϸ��ϴ� ������� �˻�
	if (pushNode->_preCode != _myCode)
		return false;

	// ���� �޸� ���п� �ڵ�� ��ȯ
	pushNode->_postCode = ~(pushNode->_postCode);

	InterlockedDecrement64((PLONG64)&_useCnt);
#else
	BLOCK* pushNode = reinterpret_cast<BLOCK*>(data);
#endif // !MEMORYPOOL_MODE_RELEASE

	// �Ҹ��� ȣ�� ���� ����
	if (_isPlacementNew)
		data->~T();

	// �޸�Ǯ�� ��ȯ�� ��ϵ��� �����ϴ� ���ÿ� PUSH
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
