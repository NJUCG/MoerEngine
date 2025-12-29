# math

[Base](#baseh)

[Matrix](#matrixh)

[Function](#functionh)

[Quaternion](#quaternionh)

[Transform](#transformh)

## Base.h
1. `Vector<T, size>`：基本向量类型，同时定义了常用类型的别名例如：三维浮点向量`Vector3f`。
   
    - 创建：

      ```c++
      Vector<unsigned int, 4> default_vec; // 默认创建，元素值为0
      // 从基础类型创建
      auto window_size = Vector2i(1920, 1080);
      auto translation = Vector3f(10.f, 2.5f, 1.f);
      auto scaling = Vector3f(2.5f); // 三个值均为2.5
      // 从向量类型创建
      auto int3_value = Vector3i(1, 2, 3);
      auto int2_value = Vector2i(int3_value);
      auto float3_value = Vector3f(int3_value);
      ```
    
   - 访问：
   
     ```c++
     auto window_size = Vector2i(1920, 1080);
     window_size.x == 1920;    // 使用x\y
     window_size[1] == 1080;   // 使用[]运算符
     window_size.e[0] == 1920; // 使用内置e
     ```
   
2. `Angle`：弧度制的角

    - 创建：

      ```c++
      Angle angle; // 默认值为0
      angle.SetDegree(90.f); // 使用角度设置值
      angle.SetDegree(0.5f * 3.1415926f); // 使用弧度设置值
      ```

    - 访问：

      ```c++
      Angle angle;
      angle.SetDegree(90.f);
      angle.GetDegree() == 90.f;
      angle.GetRadian() == 1.570796f;
      ```

## Matrix.h

`Matrix<T, row, col>`：基本**行主序**矩阵类型，同时定义了常用类型的别名例如：3x3矩阵`Matrix3x3f`。

- 创建：

  ```c++
  Matrix<int, 2, 2> m2x2i; // 默认元素均为0
  // 从基础类型创建
  auto m2x2ui = Matrix2x2ui(1, 2, 3, 4);
  float f4[4] = {1.f, 2.f, 3.f, 4.f};
  Matrix2x2f m2x2f(f4);
  // 从向量类型创建
  Vector3f r0(1, 2, 3), r1(4, 5, 6), r2(7, 8, 9);
  Matrix3x3f m3x3f(r0, r1, r2);
  // 创建单位矩阵
  auto identity = Matrix<double, 3, 3>::Identity();
  ```

- 访问：

  ```c++
  Vector3f r0(1, 2, 3), r1(4, 5, 6), r2(7, 8, 9);
  Matrix3x3f m(r0, r1, r2);
  // 按行顺序访问
  m.r0 == m[0] == m.re[0] == Vector3f(1, 2, 3);
  // 按列顺序访问
  m.GetColumn(2) == Vector3f(3, 6, 9);
  ```

## Function.h

提供向量类型和矩阵类型的通用运算函数。

- 向量：

  - 基本运算
  
      ```c++
      Vector3f vec1(1.f, -1.f, 0.f);
      Vector3f vec2(0.8f, 0.5f, -0.5f);
      Vector3i vec3(2, -2, 1);
      // Min/Max
      Min(vec1, vec2) == Vector3f(0.8f, -1.f, -0.5f);
      Max(vec2, vec1) == Vector3f(1.f, 0.5f, 0.f);
      Max(vec3, vec2) // error! 类型不匹配
      Max(Vector3f(vec3), vec2) == Vector3f(2.f, 0.5f, 1.f);
      // 四则运算
      vec1 + vec2 == Vector3f(1.8f, -0.5f, -0.5f);
      vec1 - vec2 == Vector3f(0.2f, -1.5f, 0.5f);
      vec1 * vec2 == Vector3f(0.8f, -0.5f, 0.f);
      vec1 / vec2 == Vector3f(1.25f, -2.f, 0.f);
      // 不同向量类型的运算结果与左边类型保持一致
      vec3 + vec2 == Vector3i(2, -1, 0);
      vec3 - vec2 == Vector3i(1, -2, 1);
      vec3 * vec2 == Vector3i(1, -1, 0);
      vec3 / vec2 == Vector3i(2, -4, -2);
      // 与基础类型运算
      vec1 + 1 == Vector3f(2.f, 0.f, 1.f);   // == 1 + vec1
      vec1 - 1. == Vector3f(0.f, -2.f, -1.f);// == 1 - vec1
      vec1 * 1u == Vector3f(1.f, -1.f, 0.f); // == 1 * vec1
      vec1 / 1.f == Vector3f(1.f, -1.f, 0.f);// == 1 / vec1
      ```
      
  - 渲染中常用的方法：
  
      - `ApproxEqual`：由于浮点误差的存在，提供在一定容忍度的情况下，判断两个变量是否相等。
      - `Abs`：向量各元素分别取绝对值。
      - `Lerp(a, b, t)`：线性插值`a + t * (b - a)`。
      - `Pow(base, power)`：分别计算base中元素的power次幂。
      - `Clamp(v, a, b)`：将v截断在区间`[a,b]`之间。
      - `Dot`：计算向量点乘。
      - `Length`：计算向量长度。
      - `Normalizef`：返回归一化后的向量。
      - `Cross`：计算向量叉乘。
      - `Reflect`：计算向量的反射方向。
  
- 矩阵：
  
  - 基本运算
  
    ```C++
    Matrix<float, 2, 3> m1;
    Matrix<float, 3, 2> m2;
    // 矩阵乘法
    m1 * m2 == Matrix<float, 2, 2>(Dot(m1.r0, m2.GetColumn(0)), Dot(m1.r0, m2.GetColumn(1)),
                                   Dot(m1.r1, m2.GetColumn(0)), Dot(m1.r1, m2.GetColumn(1)));
    // 与向量相乘
    Vector<int, 3> v1;
    m1 * v1 == Vector<float, 2>(Dot(m1.r0, v1), Dot(m1.r0, v1));
    Vector<float, 2> v2;
    v2 * m1 == Vector<float, 3>(Dot(v2, m1.GetColumn(0)), Dot(v2, m1.GetColumn(1)), Dot(v2, m1.GetColumn(2)))
  
  - 常用方法：
  
    - `Transpose`：取转置矩阵。
  
    - `Inverse`：矩阵取反。
  
    - `GetDiagonal2x2`：获取矩阵对角线上的2x2子矩阵。
  
      ```c++
      Matrix<int, 3, 3> m1(1, 2, 3, 4, 5, 6, 7, 8, 9);
      GetDiagonal2x2(m1) == Matrix<int, 2, 2>(1, 2, 4, 5);
      ```
  
    - `GetDiagonal3x3`：获取矩阵对角线上的3x3子矩阵。
  
    - `FillDiagonal3x3`：将2x2矩阵填补成3x3矩阵。
  
      ```c++
      Matrix<int, 2, 2> m1(1, 2, 3, 4);
      FillDiagonal3x3(m1, 1) == Matrix<int, 3, 3>(1, 2, 0, 3, 4, 0, 0, 0, 1);
      ```
  
    - `FillDiagonal4x4`：将2x2或3x3矩阵填补成4x4矩阵。
  
    - `MakeLookatViewMatrixRH`：计算右手系下的view矩阵。
  
    - `MakeLookatToWorldMatrixRH`：计算右手系下的to_world矩阵，是view矩阵的逆矩阵。
  
    - `MakeLookatViewMatrixWithInverseRH`：同时计算view和to_world矩阵（计算量比分别调用更小）。
  
    - `QDUDecomposition`：对3x3矩阵进行QDU分解，用于分解Transform矩阵。
  
    - `MakePerspectiveMatrixRH`：计算透视矩阵。
  

## Quaternion.h

`Quaternion`类型提供四元数的基本操作，其中用于初始化四元数的`Vector4f`必须保证经过归一化。

功能：

- 四元数与旋转矩阵的互相转换；
- 四元数与旋转轴、转动角的互相转换；
- 旋转逆变换；
- 四元数基本运算；
- 旋转球面插值`SLerp`；
- 旋转标准线性插值`NLerp`。

数学约定：

设旋转轴为$\vec{u}=|u_x, u_y, u_z|$，旋转角为$\theta$（弧度制），目标向量为$\vec{v}=|v_x, v_y, v_z|$，则：向量$\vec{v}$在旋转轴$\vec{u}$山旋转$\theta$角的四元数可表示为$q=(\cos(\frac{\theta}{2}),\sin(\frac{\theta}{2})*\vec{u})$。

在引擎中由基本向量类型`Vector4f`存储四元数内部数值，即：

```c++
vec.w = cos(0.5f * theta);
vec.x = sin(0.5f * theta) * u.x;
vec.y = sin(0.5f * theta) * u.y;
vec.z = sin(0.5f * theta) * u.z;
```

向量$\vec{v}$在四元数$q$上的旋转可由以下公式计算：
$$
q*q_{\vec{v}}*q^{-1}=\begin{bmatrix}1 & 0 & 0 & 0\\
0 & 1-2cc-2dd & 2bc-2ad&2ac+2bd\\
0&2bc+2ad&1-2bb-2dd&2cd-2ab\\
0&2bd-2ac&2ab+2cd&1-2bb-2cc\end{bmatrix}\cdot\begin{bmatrix}0\\v_x\\v_y\\v_z\end{bmatrix}
$$
其中$*$为四元数乘法，$q_\vec{v}=(0,\vec{v})$为向量$\vec{v}$的四元数形式。，$a$等于`vec.x`，$b$等于`vec.y`，$c$等于`vec.z`，$d$等于`vec.w`。

由于四元数表示的旋转轴经过原点，因此对于点A的旋转等价于对向量OA的旋转，其中O为原点。

## Transform.h

`Transform`类型提供三维空间的基本变换：仿射变换和透视变换。

功能：

- 构建物体的仿射变换；
- 构建相机的视图变换和透视变换；
- 逆变换；
- 仿射变换分解；
- 向量变换；
- 坐标变换（会除w）。
