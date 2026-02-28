#ifndef TYPE_CODE_H_
#define TYPE_CODE_H_

// -1 代表复合类型
enum TypeCode
{
	Empty = 0,
	Boolean,    // 1
	Char,       // 2
	Int16,      // 3
	UInt16,     // 4
	Int32,      // 5
	UInt32,     // 6
	Int64,      // 7
	UInt64,     // 8
	Single,     // 9
	Double,     // 10
	String,     // 11  PodString<N>, 具体容量由 FieldInfo.size 区分
	Struct,     // 12  嵌套 struct（限一层），具体布局由 FieldInfo.struct_info 指向
};

#endif // TYPE_CODE_H_
