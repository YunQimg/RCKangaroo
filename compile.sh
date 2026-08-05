# 1. 安装 CUDA Toolkit 12.8（如果尚未安装）
# 可从 NVIDIA 官网下载，或使用包管理器安装
git clean -fd

# 2. 创建构建目录
mkdir build && cd build

# 3. 配置 CMake
cmake ..

# 4. 编译
cmake --build . -j$(nproc)

# 5. 可执行文件位于 build/bin/rckangaroo
cd ..
cp ./build/bin/rckangaroo ./
