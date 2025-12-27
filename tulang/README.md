Tua 语言 - 极简语言
类似Lua语言，具有类型系统， 语法简单， 运行速度快， 运行环境小， 适合嵌入Java开发

语法定义:
- 定义变量:
let a:int = 10
a:int = 10
let a = 10
const b = 1000
const s = "hello world"
用let定义可变变量, 用const定义常量

- 类型
int: 整数
string: 字符串
bool: 布尔
float: 浮点数
char: 字符
array: 数组
map: 映射
struct: 结构体
interface: 接口
class: 类
enum: 枚举
fn: 函数
package: 包
long: 长整型
null: 空
void: 空


- 条件判断
if a > 10 || (b > 0 && c > 0) {
    //do something
}else if a < 5 {
    //do something
}else {
    //do something
}

|| 表示或者
&& 表示并且

- for循环
for i=0; i<n; i++ {
    //do something
}

- 函数定义
有返回值的函数定义:
fn demo(a:int, b:string) int {
    //do something
    return 10
}
-> 表示有返回值，后面的int是函数返回值类型
无返回值的函数定义:
fn demo(a: int) {
    //do something
}

- 函数返回值
return something

- 注释
// 表示一行注释
/* */ 可以表示多行注释

- 结构体定义
struct Role {
    name: string
    const age: string
    const type:string = "Role"
    dept: Department

    init() {
        //constructor
    }

    deinit() {
        //destructor
    }

    //fn function name
    fn getAge() {
        return this.age
    }

    fn func2() string,Error{
        return "ok",Ok()
    }
    private fn func3() {
        //private function
    }
}

- object 定义, 类似java的静态方法
object StringUtils {
    fn isEmtpy(s:string) -> boolean {
        return s == null || s == ""
    }

    private fn emtpy() {
        //do private something
    }
}

- 枚举定义
enum RoleType {
    Admin,
    User,
}

