# TinyWebServer
项目是基于 Linux 的轻量级多线程 Web 服务器，应用层实现了一个简单的HTTP 服务器，支持用户登录与静态资源访问。

* 利用同步**I/O方式模拟 Proactor 模式**，采用**线程池模式**、**非阻塞 LT**的**Epoll**技术，实现服务器的并发模型；
* 使用**状态机**解析HTTP请求报文，支持解析**GET**请求
* 实现非活跃连接的定时检测，完成服务器压力测试，实现**上万的并发连接**数据交换。

快速运行，编译后运行，并指定端口号。
```
g++ *.cpp -pthread
./a.out 10000
```
------------
* 服务器测试环境
	* Ubuntu版本Ubuntu 18.04.6
* 浏览器测试环境
	* Windows、Linux均可
	* Chrome
	* Edge
	* 其他浏览器暂无测试
* [代码详细注释版本](https://github.com/SamuelChu8848/TinyWebServe_with_Code_Comments)
* 流程图
* ![avatar](https://github.com/SamuelChu8848/TinyWebServer/blob/master/resourses/images/image4.png)
