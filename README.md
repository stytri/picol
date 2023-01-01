# picol
My refactoring of the Picol

Refactored version of the original **picol**[1][2][3] to reduce it to the stated aim of **500** lines, while also adding the missing escape handling (and changing static buffers to dynamic buffers). No existing functionality was removed or altered.

Code size was primarily reduced by:

* Eliding redundant braces.
* Converting **while** loops to **for** loops, and pulling in variable declarations &c.
* Variable/member declarations of the same type on one line.
* Single line conditionals where the consequent is a single statement.

[1] http://oldblog.antirez.com/post/picol.html]

[2] https://wiki.tcl-lang.org/page/Picol]

[3] https://news.ycombinator.com/item?id=33963918
