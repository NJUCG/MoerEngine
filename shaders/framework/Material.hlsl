#ifndef FRAMEWORK_MATERIAL_HLSL
#define FRAMEWORK_MATERIAL_HLSL

// #include "framework/Bindless.hlsl"

template <typename T>
T UnpackMaterialData(uint material_buffer_handle,uint material_index){
    ArrayBuffer buf = ArrayBuffer(material_buffer_handle);
    return buf.GetByteAddressBuffer().Load<T>(material_index);
}

#define Material_Standard_PBR 0

#endif