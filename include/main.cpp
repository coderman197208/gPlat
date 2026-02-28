#include <cstdio>
#include "podstring.h"

int main()
{
	PodString10 podstr("include123");
	std::string str = podstr.to_string();
    printf("%s 向你问好!\n", str.c_str());
    return 0;
}