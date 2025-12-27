package tua.examples

from test6 import Error, Ok
from test5 import X,*
from tua.examples.test6 import StringUtils

object StringUtils {
    fn isEmtpy(s:string) -> boolean {
        return s == null || s == ""
    }

    private fn emtpy() {
        //do private something
    }
}

struct Department  {
    name: string
}

const Role = struct {

}
struct Role {
    var name: string
    const age: string
    const type:string = "Role"
    var dept: Department

    init() {

    }

    deinit() {

    }

    fn getAge() {
        return this.age
    }

    fn func2() string,Error{
        return "ok",Ok()
    }
    private fn func3() {

    }
}

struct User {
    name: string {
        getter() string {
            return _
        }
        setter(value:string) {
            _ = value
        }

        get: {
            return _name
        }
        set: {
            _name = newValue
        }
    },
    age: int
    var role: Role
}

user.role.dept.name = "it"
user.role.name = "admin"
var role = user.role
role.name = "admin"

u1.role.name = u2.role.name


map,list,array,string,set,char,int,int8,int16,long,uint8,uint16,uint32,uint64, int64,float,double,decimal

//#test7 User
from tua.examples.test6 import User

impl User {
    fn toString() string {
        return this.name
    }
}

impl User {
    fn sayHello() string {
        return "hello " + this.name
    }
}

fn User__sayHello(this:User) -> string {
    return "hello " + this.name
}

fn main(argv:string[]) {
    println("main fn")
}

fn test(s:string) {
    var empty = StringUtils.isEmpty(s)

}

let a = 0;

let a = 0
let i:int = 0
var a = 0
var a:int = 0
var a:long = 0
var a = "hello"
var a:string

//run tua tua.examples.test6.App
object Some {}
object Any {}

object App {
    var config:string;
    var name = "Tu Demo"
    fn main(argv:string[]) {
        //main entry
        config = "yaml"
        App.config = "yaml"
    }

    fn sayHello() {
        App.name = "TuZ"
    }
}

var App = App()

App$$main(argv:string[]) {
    App.config = "yaml"
}

App$$sayHello() {
    App.name = "TuZ"
}

interface Trueable {
    fn isTrue() boolean ;
}
struct SystemCode : Trueable{
    code: int
}
impl SystemCode {
    fn isTrue() boolean {
        return false
    }
}

struct Error<T> : Trueable{
    code:int?,
    message:string?,
    data: T
}

var err = Error()

impl Error {
    fn init(t:T) {
        this.data = t
        this.code = -1
    }
    fn init() {
        this.data = null
        code = -1
    }
    fn isTrue() {
        return false
    }
}

struct Ok : Error<int> {
    
}

impl Ok {
    Ok() {
        code = 0
        data = 0
    }
    fn isTrue() boolean {
        return true
    }
}

object Trueable {
    fn isTrue() boolean;
}


object Window {
    var a:int = 10;
    const b:int = 1000;
}

Window.a = 100
var b = Window.b


struct Ok : Error<int> {
    fn init(int data) {
        this.data = data
        this.code = 0
        this.message = null
    }
    fn isTrue() boolean {
        return true
    }
}
def Ok = Error(0)
def Err = Error(-1)


impl Error : Nullable{
    init() {
        code = 1
    }
    init(code:int) {
        this.code = code
    }
    init(msg:string) {
        code = 1
        message = msg
    }
    init(code:int, msg:string, t:T) {
        this.code = code
        this.message = msg
        this.data = t
    }

    fn isNotNull() boolean {
        return code != 0
    }

    fn nullable() {
        return code == 0
    }

    fn detail() string {

    }
    private fn say() {
        println("private func say")
    }
}

for var i=0; i<100; i++ {
    a = a+i
}

println(a);

a = if true -> 'a' else 'b'
a = if device_type == "cpu" -> something else elsething
a = something if device_type == "cpu" else otherthing

a = if true ? a: b

a?.saySomething()
a.saySomething()


for v,index in list -> println(v,index)
for v,index in list -> println(v,index)

for v in list {
    println(v)
}


var name = match a {
    case 'b' -> {
        return something
    }
    case c -> helo()
    case d -> hello()
    case _ -> 
        nothing()
}

fn move(dx:int, dy:int) int -> x = x + dx,y += dy

static fn Ok() -> Error(0)

fn move(dx: int, dy: int) int throws {
    return 0,Ok()
}

fn move(dx:int = 0, dy:int) Result<int> {
    x += dx
    y += dy
    return x+y, Ok()

    return _, err
}

fn testString(w: string) {
    var s = ""
    s = "hello" + w + "-" + "world"
    println(s)
    log("{}, {}, print s {}", __FILE__, __FUNC__,__LINE__, s)
    //test6.tuzi, testString(172)
}

fn testMove() int {
    var res,err = move(dy:10, dx:1)
    var r = move(1, 10)
    assertNull(err) //if err != null return ?
    if err != null {

    }
}


assertNull(err) //assert(err.isOk(), err) 
var arr = array<T>()
var arr = int[10]{}
var n = 10
var arr = int[n]{}
var a = any[]{}


enum DemoType(name:String, age:int) {
    case Type1("type1",0)
    case Type2("type2",0)
    case Type3("type3",2)


}


DemoType.Type1.name
DemoType.Type2.age

demoType = .Type1
demoType = .Type2

fn <T extends number> process(t:T) int,Error {
    if t % 2 == 0 {
        return t, Ok()
    }else {
        return 0, Error()
    }
}

if err != null {
    panic(err)
}

assert(err)
continue(err)


fn seq() int,Error {
    var res: string[] = []
    var res = array<string>() {}
    var res = Array<String>() {}
    var res: array<string> = [0,2,12]
    var res: list<string> = []
    var res: map<string, int> = map<>()

    var res = int[]() {}
    for item in self {
        var r = unpack(process(item))
        var r, err = process(item)
        if err != null {
            continue
        }
        if err {
            continue
        }
        res.append(r)
    }
    return res,Ok()
}
 
match err {
    case Ok() -> Ok
    case Error(e) -> (e.code,e.message)
}

fn assertOk(err: Error) {
    if err == null || err.isOk() {
        return
    }else {
        exit(err, -1) //返回上一层
    }
}

fn assert(isTrue: boolean, err: Error) int,Error {
    if !isTrue {
        exitCaller(__CALLER__,__VA_ARGS__,__CALLERLINE__, err)
    }
}

fn foo(n:int) int, Error {
    if n > 10 {
        return 0,Ok()
    } else {
        return n,Error("n不大于10")
    }

    return (n,Ok()) if n > 10 else (n, Error())
}

if res=move(1,10) {

}

@Export("c")
fn position(s:Shape) (x,y) -> s.x,s.y
fn position(s:Shape) (x,y) {
    (s.x, s.y)
    return s.x,s.y
}

var x,y,err = position(s)
if err {

}


@Exportc
@ExportJava
fn position() {

}
