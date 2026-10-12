#pragma once
//
// fx/NoteAtlas.h —— 音符图集（CPU SDF 光栅化，上传 GPU 后只读）
//
// 一张横向 4 格的图集：0=♩ 1=♪ 2=♫ 3=径向柔光斑。
// 形状全部用矢量 SDF 基元（线段距离、椭圆距离）拼装，不用字体字形与图片素材。
// CPU 逐像素写入 Image（白色 RGB，alpha=覆盖率，4×4 超采样 ≈1.5px 抗锯齿过渡），
// 然后 LoadTextureFromImage + GenTextureMipmaps + 三线性过滤，上传后立即 UnloadImage。
//
#include "raylib.h"

namespace piano {
namespace fx {

class NoteAtlas {
public:
    NoteAtlas() = default;
    ~NoteAtlas();
    NoteAtlas(const NoteAtlas&) = delete;
    NoteAtlas& operator=(const NoteAtlas&) = delete;

    /// 幂等：rasterScale 与当前烘焙值一致时不做任何事。
    /// 变化时重烘焙，旧 Texture 先 Unload。返回是否发生了重烘焙。
    bool bake(float rasterScale);
    void unload();

    bool ready() const { return ready_; }
    Texture2D texture() const { return tex_; }
    int cellPx() const { return cellPx_; }              // 单元格边长（纹理像素）
    float cellLogical() const;                          // 单元格对应的逻辑像素边长
    Rectangle cellRect(int shape) const;                // shape 0..3 的源矩形
    float bakedScale() const { return bakedScale_; }

    /// 最大绘制尺寸（逻辑像素），由令牌推导，决定单元格边长。
    static float maxDrawSize();

private:
    Texture2D tex_{};
    bool  ready_ = false;
    int   cellPx_ = 0;
    float bakedScale_ = 0.0f;
};

} // namespace fx
} // namespace piano
