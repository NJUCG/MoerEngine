python/x64下载自：https://www.nuget.org/api/v2/package/python/3.12.10

相比原始包，我们做了以下修改：
1. 只保留tools文件夹，删除nuget元数据
2. 删除了和需求无关的较大子模块：
   1. DLLs/python.cat
   2. Lib/pydoc_data
   3. Lib/lib2to3