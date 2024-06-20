#pragma once

// #include "reflect/type"
#include <limits>
#include "reflect/reflection.generated.hpp"

namespace test {
struct Parent {
    int abc;
};
namespace inner {
struct Oops {
    int field1;
    const char* field2 = "abc";
    bool field3 = true;
};
union Yeppp {};
}
}

using AliasType1 = test::Parent;
typedef test::Parent AliasType2;
using Yeppp = test::inner::Oops;


namespace zeno
{
    struct ZRECORD(测试1=真, 组测试=(组1, Group 2, Group 3), 123=true, 456) IAmPrimitve {
        IAmPrimitve(const IAmPrimitve&) = default;

        signed int i32 = 10086;

        ZNODE(Name="做些事") 
        void DoSomething(int value) const;
    };

    struct ZRECORD(DisplayName="你好") 基类测试 : public AliasType2 {
        
        基类测试() = default;

        const char* 字段1 = "Hello World";
    };
}

