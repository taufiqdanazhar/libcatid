/*
	Copyright (c) 2011 Christopher A. Taylor.  All rights reserved.

	Redistribution and use in source and binary forms, with or without
	modification, are permitted provided that the following conditions are met:

	* Redistributions of source code must retain the above copyright notice,
	  this list of conditions and the following disclaimer.
	* Redistributions in binary form must reproduce the above copyright notice,
	  this list of conditions and the following disclaimer in the documentation
	  and/or other materials provided with the distribution.
	* Neither the name of LibCat nor the names of its contributors may be used
	  to endorse or promote products derived from this software without
	  specific prior written permission.

	THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
	AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
	IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
	ARE DISCLAIMED.  IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
	LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
	CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
	SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
	INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
	CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
	ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
	POSSIBILITY OF SUCH DAMAGE.
*/

#ifndef CAT_DOUBLY_LINKED_LISTS_HPP
#define CAT_DOUBLY_LINKED_LISTS_HPP

#include <cat/Platform.hpp>

// The point of this code is to generate fast and correct implementations of
// linked lists that introduce a minimal amount of overhead.

namespace cat {
	

/*
	Forward-only doubly linked list

	Macros for correct implementation of a linked list,
	where the next/prev variables are members of the objects in
	the list.  The names of the next/prev member variables must
	be provided in the arguments to the macros.

	To iterate through the list, use code like this:

		for (Node *item = head; item; item = item->next)

	Can insert at the head of the list, pushing the previous head
	down the list.

	Can erase an item in the list given a pointer to the item.

	Operations are all O(1).
	Not thread-safe.
*/

/*
	Clear the list without releasing memory, useful for startup.

	head: Pointer to the head of the list
*/
#define CAT_FDLL_CLEAR(head)	\
{							\
	head = 0;				\
}

/*
	Push object to the front of the list.

	head: Pointer to the head of the list
	obj: Pointer to object that will be inserted at front
	next: Name of member variable pointer to next list item
	prev: Name of member variable pointer to previous list item
*/
#define CAT_FDLL_PUSH_FRONT(head, obj, next, prev)	\
{													\
	(obj)->prev = 0;								\
	(obj)->next = head;								\
													\
	if (head) (head)->prev = obj;					\
													\
	head = obj;										\
}

/*
	Insert an item directly before another item.

	head: Pointer to the head of the list
	obj: Pointer to object that will be inserted before another item
	another: List item that will be after the inserted item
	next: Name of member variable pointer to next list item
	prev: Name of member variable pointer to previous list item
*/
#define CAT_FDLL_INSERT_BEFORE(head, obj, another, next, prev)	\
{																\
	(obj)->prev = (another)->prev;								\
	(obj)->next = another;										\
																\
	if (!(another)->prev) head = obj;							\
																\
	(another)->prev = obj;										\
}

/*
	Insert an item directly after another item.

	head: Pointer to the head of the list
	obj: Pointer to object that will be inserted after another item
	another: List item that will be before the inserted item
	next: Name of member variable pointer to next list item
	prev: Name of member variable pointer to previous list item
*/
#define CAT_FDLL_INSERT_AFTER(head, obj, another, next, prev)	\
{																\
	(obj)->next = (another)->next;								\
	(obj)->prev = another;										\
																\
	(another)->next = obj;										\
}

/*
	Erase object from the list, does not release memory.

	head: Pointer to the head of the list
	obj: Pointer to object that will be inserted at front
	next: Name of member variable pointer to next list item
	prev: Name of member variable pointer to previous list item
*/
#define CAT_FDLL_ERASE(head, obj, next, prev)				\
{															\
	if ((obj)->prev)	((obj)->prev)->next = (obj)->next;	\
	else				head = (obj)->next;					\
															\
	if ((obj)->next)	((obj)->next)->prev = (obj)->prev;	\
}


/*
	Bi-directional doubly linked list

	Macros for correct implementation of a doubly linked list,
	where the next/prev variables are members of the objects in
	the list.  The names of the next/prev member variables must
	be provided in the arguments to the macros.

	To iterate forward through the list, use code like this:

		for (Node *item = head; item; item = item->next)

	To iterate backward through the list, use code like this:

		for (Node *item = tail; item; item = item->prev)

	Can insert at the head of the list, pushing the previous head
	down the list.

	Can insert at the tail of the list, appending to the list.

	Can erase an item in the list given a pointer to the item.

	Operations are all O(1).
	Not thread-safe.
*/

/*
	Clear the list without releasing memory, useful for startup.

	head: Pointer to the head of the list
	tail: Pointer to the tail of the list
*/
#define CAT_BDLL_CLEAR(head, tail)	\
{									\
	head = 0;						\
	tail = 0;						\
}

/*
	Push object to the front of the list.

	head: Pointer to the head of the list
	tail: Pointer to the tail of the list
	obj: Pointer to object that will be inserted at front
	next: Name of member variable pointer to next list item
	prev: Name of member variable pointer to previous list item
*/
#define CAT_BDLL_PUSH_FRONT(head, tail, obj, next, prev)	\
{														\
	(obj)->prev = 0;									\
	(obj)->next = head;									\
														\
	if (head)	(head)->prev = obj;						\
	else		tail = obj;								\
														\
	head = obj;											\
}

/*
	Push object to the back of the list, appending to it.

	head: Pointer to the head of the list
	tail: Pointer to the tail of the list
	obj: Pointer to object that will be inserted at front
	next: Name of member variable pointer to next list item
	prev: Name of member variable pointer to previous list item
*/
#define CAT_BDLL_PUSH_BACK(head, tail, obj, next, prev)	\
{														\
	(obj)->prev = tail;									\
	(obj)->next = 0;									\
														\
	if (tail)	(tail)->next = obj;						\
	else		head = obj;								\
														\
	tail = obj;											\
}

/*
	Insert an item directly before another item.

	head: Pointer to the head of the list
	tail: Pointer to the tail of the list
	obj: Pointer to object that will be inserted before another item
	another: List item that will be after the inserted item
	next: Name of member variable pointer to next list item
	prev: Name of member variable pointer to previous list item
*/
#define CAT_BDLL_INSERT_BEFORE(head, tail, obj, another, next, prev)	\
{																\
	(obj)->prev = (another)->prev;								\
	(obj)->next = another;										\
																\
	if (!(another)->prev) head = obj;							\
																\
	(another)->prev = obj;										\
}

/*
	Insert an item directly after another item.

	head: Pointer to the head of the list
	tail: Pointer to the tail of the list
	obj: Pointer to object that will be inserted after another item
	another: List item that will be before the inserted item
	next: Name of member variable pointer to next list item
	prev: Name of member variable pointer to previous list item
*/
#define CAT_BDLL_INSERT_AFTER(head, tail, obj, another, next, prev)	\
{																\
	(obj)->next = (another)->next;								\
	(obj)->prev = another;										\
																\
	if (!(another)->next) tail = obj;							\
																\
	(another)->next = obj;										\
}

/*
	Erase object from the list, does not release memory.

	head: Pointer to the head of the list
	tail: Pointer to the tail of the list
	obj: Pointer to object that will be inserted at front
	next: Name of member variable pointer to next list item
	prev: Name of member variable pointer to previous list item
*/
#define CAT_BDLL_ERASE(head, tail, obj, next, prev)				\
{																\
	if ((obj)->prev)	((obj)->prev)->next = (obj)->next;		\
	else				head = (obj)->next;						\
																\
	if ((obj)->next)	((obj)->next)->prev = (obj)->prev;		\
	else				tail = (obj)->prev;						\
}


/*
	Derive doubly-linked list items from this base class
	to wrap using the macros above.
*/
class DListItem
{
	friend class DList;
	friend class DListForward;
	friend class DListIteratorBase;

private:
	DListItem *_next, *_prev;
};


// Internal class
class DListIteratorBase
{
protected:
	DListItem *_item;

	CAT_INLINE void IterateNext()
	{
		_item = _item->_next;
	}

	CAT_INLINE void IteratePrev()
	{
		_item = _item->_prev;
	}
};


/*
	Access the linked list through this type.
*/
class DListForward
{
	DListItem *_head;

public:
	DListForward();

	CAT_INLINE DListItem *head() { return _head; }

	void PushFront(DListItem *item);
	void InsertBefore(DListItem *item, DListItem *at);
	void InsertAfter(DListItem *item, DListItem *at);
	void Erase(DListItem *item);

	/*
		When iterating forward through the list,
		use the following mechanism:

		for (DListForward::Iterator<MyObject> ii = list.head(); ii; ++ii)
	*/
	template<class T>
	class Iterator : DListIteratorBase
	{
		CAT_INLINE Iterator() {}

	public:
		CAT_INLINE Iterator(DListItem *item)
		{
			_item = item;
		}

		CAT_INLINE Iterator &operator=(DListItem *item)
		{
			_item = item;
			return *this;
		}

		CAT_INLINE operator T *()
		{
			return static_cast<T*>( _item );
		}

		CAT_INLINE T *operator->()
		{
			return static_cast<T*>( _item );
		}

		CAT_INLINE Iterator &operator++() // pre-increment
		{
			IterateNext();
			return *this;
		}

		CAT_INLINE Iterator &operator++(int) // post-increment
		{
			IterateNext();
			return *this;
		}
	};
};


/*
	Access the linked list through this type.
*/
class DList
{
	DListItem *_head, *_tail;

public:
	DList();

	CAT_INLINE DListItem *head() { return _head; }
	CAT_INLINE DListItem *tail() { return _tail; }

	void PushFront(DListItem *item);
	void PushBack(DListItem *item);
	void InsertBefore(DListItem *item, DListItem *at);
	void InsertAfter(DListItem *item, DListItem *at);
	void Erase(DListItem *item);

	/*
		When iterating forward through the list,
		use the following mechanism:

		for (DList::Iterator<MyObject> ii = list.head(); ii; ++ii)
	*/
	template<class T>
	class Iterator : DListIteratorBase
	{
		CAT_INLINE Iterator() {}

	public:
		CAT_INLINE Iterator(DListItem *item)
		{
			_item = item;
		}

		CAT_INLINE Iterator &operator=(DListItem *item)
		{
			_item = item;
			return *this;
		}

		CAT_INLINE operator T *()
		{
			return static_cast<T*>( _item );
		}

		CAT_INLINE T *operator->()
		{
			return static_cast<T*>( _item );
		}

		CAT_INLINE Iterator &operator++() // pre-increment
		{
			IterateNext();
			return *this;
		}

		CAT_INLINE Iterator &operator++(int) // post-increment
		{
			IterateNext();
			return *this;
		}

		CAT_INLINE Iterator &operator--() // pre-decrement
		{
			IteratePrev();
			return *this;
		}

		CAT_INLINE Iterator &operator--(int) // post-decrement
		{
			IteratePrev();
			return *this;
		}
	};
};


} // namespace cat

#endif // CAT_DOUBLY_LINKED_LISTS_HPP