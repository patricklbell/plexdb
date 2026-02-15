implements a b+tree
references:
- https://bobson.ludost.net/books/algo/book6/chap19.htm
- https://planetscale.com/blog/btrees-and-database-indexes
- https://www.cs.emory.edu/~cheung/Courses/554/Syllabus/3-index/B-tree=delete3.html
internal keys are the minimum value for the subtree at the next index

TODO:
- lock and unlock pages, since currently this relies on collision not 
  occuring when holding two pages, which would invalidate the pointer
- configurable left-right bias on split
- append-right, append-left (or as bias)