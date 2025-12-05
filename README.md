![](1snap.png)

# 0verhex
ncurses hex editor made for having fun

# Compile

```bash
make -j8  # 8 cores/threads to use in parallel compile
sudo/doas make install
```

# Requirements

In Debian it's `sudo apt install libncurses5-dev libncursesw5-dev `, in your other OS's search for `lib ncurses`.

### To-do:

1. `Completed. But still needs some testing.` ~Add undo/redo functions and don't store the entire file many times for undo/redo, but only the parts that was exchanged.~
