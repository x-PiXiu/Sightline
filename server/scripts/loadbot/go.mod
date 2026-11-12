module sightline/loadbot

// ep09 e2e（multiroom_test.go）：本地工具链 go1.23.4——原 1.26.1 高于工具链会拒编译；
// go 行=最低版本声明，1.26 工具链编译本模块同样兼容（loadbot 代码无 1.26 特性）。
go 1.23
