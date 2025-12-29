# misc

[Timer](#timer.h)

# Timer.h
提供基本的cpu端计时功能。
示例：
```c++
Timer t;
t.Start();
// do something
t.Stop();

t.ElapsedMilliseconds(); // 获取从Start()到Stop()的执行时间，单位是毫秒
t.ElapsedSeconds(); // 获取从Start()到Stop()的执行时间，单位是秒
```