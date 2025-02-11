# ThreadPool-based-on-cpp11
线程数量可根据任务数量动态增加或动态回收，设计既能满足高响应，又能节省线程资源。
支持任意函数或参数的传递
#example
void HelloWorld()
{
  std::cout<<"Hello World!"<<std::endl;
}
int sum(int a,int b)
{
  return a+b;
}
int main()
{
  threadpool pool;
  pool.start(2);
  pool.submitTask(HelloWorld);
  future<int> res = pool.submitTask(sum,10,20);
  cout<<res.get()<<endl;
  
  return 0;
}
