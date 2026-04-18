// Copyright (c) 2025, The Regents of the University of California (Regents)
// See LICENSE for license details

#ifndef GROUPED_STACK_H_
#define GROUPED_STACK_H_

#include <cassert>
#include <span>
#include <vector>


/*
PivotScale
File:   GroupedStack
Author: Scott Beamer, Amogh Lonkar

Provides abstraction of a stack where objects are pushed into a frame, and
can provide an iterator (via std::span) to that frame ignoring the rest of
the contents. New frames can be created with create_new_frame() and popped
with pop_frame().

NOTE: For stability of the output of last_frame_iter, ensure no insertions
(via push_back) or you have used reserve(new_size) to prevent the need for
realloc.
*/

/*
Modified GroupedStack, no .resize() calls 
*/

template <typename T_>
class GroupedStack {
  std::vector<T_> elems_;
  std::vector<size_t> starts_;
  size_t top_ = 0;  // logical end; elems_[top_..capacity) is dead storage

 public:
  GroupedStack() {}

  void reserve(int num_elems) {
    if (num_elems > static_cast<int>(elems_.size())){
      elems_.resize(num_elems);  
      starts_.reserve(num_elems);
    }
  }

  void create_new_frame() {
    starts_.push_back(top_);
  }

  void push_back(T_ new_elem) {
    assert(top_ < elems_.size() && "More elements being inserted than reserved size");
    elems_[top_++] = new_elem;
  }

  std::span<const T_> last_frame_iter() const {
    /* first, last */
    // return std::span(elems_.begin() + starts_.back(), elems_.begin() + top_);
    /* first, count */
    size_t b = starts_.back();
    return std::span(elems_.data() + b, top_ - b);
  }

  // O(1): just rewind top_, no deallocation
  void pop_frame() {
    top_ = starts_.back();
    starts_.pop_back();
  }

  void clear() {
    starts_.clear();
    top_ = 0;
  }
};

#endif  // GROUPED_STACK_H_
