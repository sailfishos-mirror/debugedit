// Generates approx 65548 sections ( 32768 comdat groups + 32770 .text )
// when build as an object file
// https://sourceware.org/bugzilla/show_bug.cgi?id=33819
// Testcase suggested by James Abbatiello <abbeyj@gmail.com>>

template <int I> void foo() { }

#define F0 foo<__COUNTER__>();
#define F1 F0 F0
#define F2 F1 F1
#define F3 F2 F2
#define F4 F3 F3
#define F5 F4 F4
#define F6 F5 F5
#define F7 F6 F6
#define F8 F7 F7
#define F9 F8 F8
#define F10 F9 F9
#define F11 F10 F10
#define F12 F11 F11
#define F13 F12 F12
#define F14 F13 F13
#define F15 F14 F14

int main()
{
  F15;
  return 0;
}
