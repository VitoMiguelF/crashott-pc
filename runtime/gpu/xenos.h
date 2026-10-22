#pragma once

// Xenos register indices, enums and bit fields used by the renderer. Register numbers, field
// layouts and enum values follow Xenia's register_table.inc / registers.h / xenos.h (BSD
// license, https://github.com/xenia-project/xenia); only what the renderer reads is defined.

#include <cstdint>

namespace xenos
{
    // Registers (index N lives at GPU_MMIO_BEGIN + N * 4).
    enum Register : uint32_t
    {
        REG_RB_SURFACE_INFO = 0x2000,
        REG_RB_COLOR_INFO = 0x2001,
        REG_RB_DEPTH_INFO = 0x2002,
        REG_RB_COLOR1_INFO = 0x2003,
        REG_RB_COLOR2_INFO = 0x2004,
        REG_RB_COLOR3_INFO = 0x2005,
        REG_PA_SC_SCREEN_SCISSOR_TL = 0x200E,
        REG_PA_SC_SCREEN_SCISSOR_BR = 0x200F,
        REG_PA_SC_WINDOW_OFFSET = 0x2080,
        REG_PA_SC_WINDOW_SCISSOR_TL = 0x2081,
        REG_PA_SC_WINDOW_SCISSOR_BR = 0x2082,
        REG_VGT_MAX_VTX_INDX = 0x2100,
        REG_VGT_MIN_VTX_INDX = 0x2101,
        REG_VGT_INDX_OFFSET = 0x2102,
        REG_VGT_MULTI_PRIM_IB_RESET_INDX = 0x2103,
        REG_RB_COLOR_MASK = 0x2104,
        REG_RB_BLEND_RED = 0x2105,
        REG_RB_STENCILREFMASK_BF = 0x210C,
        REG_RB_STENCILREFMASK = 0x210D,
        REG_RB_ALPHA_REF = 0x210E,
        REG_PA_CL_VPORT_XSCALE = 0x210F,
        REG_PA_CL_VPORT_XOFFSET = 0x2110,
        REG_PA_CL_VPORT_YSCALE = 0x2111,
        REG_PA_CL_VPORT_YOFFSET = 0x2112,
        REG_PA_CL_VPORT_ZSCALE = 0x2113,
        REG_PA_CL_VPORT_ZOFFSET = 0x2114,
        REG_SQ_PROGRAM_CNTL = 0x2180,
        REG_SQ_CONTEXT_MISC = 0x2181,
        REG_VGT_DMA_BASE = 0x21FA,
        REG_VGT_DMA_SIZE = 0x21FB,
        REG_VGT_DRAW_INITIATOR = 0x21FC,
        REG_RB_DEPTHCONTROL = 0x2200,
        REG_RB_BLENDCONTROL0 = 0x2201,
        REG_RB_COLORCONTROL = 0x2202,
        REG_PA_CL_CLIP_CNTL = 0x2204,
        REG_PA_SU_SC_MODE_CNTL = 0x2205,
        REG_PA_CL_VTE_CNTL = 0x2206,
        REG_RB_MODECONTROL = 0x2208,
        REG_RB_BLENDCONTROL1 = 0x2209,
        REG_RB_BLENDCONTROL2 = 0x220A,
        REG_RB_BLENDCONTROL3 = 0x220B,
        REG_PA_SU_POINT_SIZE = 0x2280,
        REG_PA_SC_VIZ_QUERY = 0x2293,
        REG_PA_SU_VTX_CNTL = 0x2302,
        REG_SQ_VS_CONST = 0x2307,
        REG_SQ_PS_CONST = 0x2308,
        REG_RB_COPY_CONTROL = 0x2318,
        REG_RB_COPY_DEST_BASE = 0x2319,
        REG_RB_COPY_DEST_PITCH = 0x231A,
        REG_RB_COPY_DEST_INFO = 0x231B,
        REG_RB_DEPTH_CLEAR = 0x231D,
        REG_RB_COLOR_CLEAR = 0x231E,
        REG_RB_COLOR_CLEAR_LO = 0x231F,
        REG_PA_SU_POLY_OFFSET_FRONT_SCALE = 0x2380,
        REG_PA_SU_POLY_OFFSET_FRONT_OFFSET = 0x2381,
        REG_PA_SU_POLY_OFFSET_BACK_SCALE = 0x2382,
        REG_PA_SU_POLY_OFFSET_BACK_OFFSET = 0x2383,

        REG_SHADER_CONSTANT_000_X = 0x4000, // 512 float4
        REG_SHADER_CONSTANT_FETCH_00_0 = 0x4800, // 32 x 6 dwords
        REG_SHADER_CONSTANT_BOOL_000_031 = 0x4900, // 8 dwords
        REG_SHADER_CONSTANT_LOOP_00 = 0x4908, // 32 dwords
    };

    inline constexpr uint32_t ColorInfoRegister(uint32_t index)
    {
        constexpr uint32_t registers[4] = { REG_RB_COLOR_INFO, REG_RB_COLOR1_INFO, REG_RB_COLOR2_INFO, REG_RB_COLOR3_INFO };
        return registers[index & 3];
    }

    inline constexpr uint32_t BlendControlRegister(uint32_t index)
    {
        constexpr uint32_t registers[4] = { REG_RB_BLENDCONTROL0, REG_RB_BLENDCONTROL1, REG_RB_BLENDCONTROL2, REG_RB_BLENDCONTROL3 };
        return registers[index & 3];
    }

    // Extracts `width` bits at `offset`.
    inline constexpr uint32_t Bits(uint32_t value, uint32_t offset, uint32_t width)
    {
        return (value >> offset) & ((width >= 32) ? ~0u : ((1u << width) - 1));
    }

    // Sign-extends `width` bits at `offset`.
    inline constexpr int32_t SignedBits(uint32_t value, uint32_t offset, uint32_t width)
    {
        return int32_t(value << (32 - offset - width)) >> (32 - width);
    }

    enum class PrimitiveType : uint32_t
    {
        None = 0x00,
        PointList = 0x01,
        LineList = 0x02,
        LineStrip = 0x03,
        TriangleList = 0x04,
        TriangleFan = 0x05,
        TriangleStrip = 0x06,
        TriangleWithWFlags = 0x07,
        RectangleList = 0x08,
        LineLoop = 0x0C,
        QuadList = 0x0D,
        QuadStrip = 0x0E,
        Polygon = 0x0F,
    };

    enum class SourceSelect : uint32_t
    {
        DMA = 0,
        Immediate = 1,
        AutoIndex = 2,
    };

    enum class Endian : uint32_t
    {
        None = 0,
        k8in16 = 1,
        k8in32 = 2,
        k16in32 = 3,
    };

    inline uint32_t GpuSwap(uint32_t value, uint32_t endian)
    {
        switch (endian & 3)
        {
        case 1: return ((value >> 8) & 0x00FF00FF) | ((value << 8) & 0xFF00FF00);
        case 2: return __builtin_bswap32(value);
        case 3: return (value >> 16) | (value << 16);
        default: return value;
        }
    }

    enum class EdramMode : uint32_t
    {
        NoOperation = 0,
        ColorDepth = 4,
        DepthOnly = 5,
        Copy = 6,
    };

    enum class MsaaSamples : uint32_t
    {
        k1X = 0,
        k2X = 1,
        k4X = 2,
    };

    enum class ColorRenderTargetFormat : uint32_t
    {
        k_8_8_8_8 = 0,
        k_8_8_8_8_GAMMA = 1,
        k_2_10_10_10 = 2,
        k_2_10_10_10_FLOAT = 3,
        k_16_16 = 4,
        k_16_16_16_16 = 5,
        k_16_16_FLOAT = 6,
        k_16_16_16_16_FLOAT = 7,
        k_2_10_10_10_AS_10_10_10_10 = 10,
        k_2_10_10_10_FLOAT_AS_16_16_16_16 = 12,
        k_32_FLOAT = 14,
        k_32_32_FLOAT = 15,
    };

    inline bool IsColorFormat64bpp(uint32_t format)
    {
        return format == uint32_t(ColorRenderTargetFormat::k_16_16_16_16) ||
               format == uint32_t(ColorRenderTargetFormat::k_16_16_16_16_FLOAT) ||
               format == uint32_t(ColorRenderTargetFormat::k_32_32_FLOAT);
    }

    enum class DepthRenderTargetFormat : uint32_t
    {
        kD24S8 = 0,
        kD24FS8 = 1,
    };

    enum class TextureFormat : uint32_t
    {
        k_1_REVERSE = 0,
        k_1 = 1,
        k_8 = 2,
        k_1_5_5_5 = 3,
        k_5_6_5 = 4,
        k_6_5_5 = 5,
        k_8_8_8_8 = 6,
        k_2_10_10_10 = 7,
        k_8_A = 8,
        k_8_B = 9,
        k_8_8 = 10,
        k_Cr_Y1_Cb_Y0_REP = 11,
        k_Y1_Cr_Y0_Cb_REP = 12,
        k_16_16_EDRAM = 13,
        k_8_8_8_8_A = 14,
        k_4_4_4_4 = 15,
        k_10_11_11 = 16,
        k_11_11_10 = 17,
        k_DXT1 = 18,
        k_DXT2_3 = 19,
        k_DXT4_5 = 20,
        k_16_16_16_16_EDRAM = 21,
        k_24_8 = 22,
        k_24_8_FLOAT = 23,
        k_16 = 24,
        k_16_16 = 25,
        k_16_16_16_16 = 26,
        k_16_EXPAND = 27,
        k_16_16_EXPAND = 28,
        k_16_16_16_16_EXPAND = 29,
        k_16_FLOAT = 30,
        k_16_16_FLOAT = 31,
        k_16_16_16_16_FLOAT = 32,
        k_32 = 33,
        k_32_32 = 34,
        k_32_32_32_32 = 35,
        k_32_FLOAT = 36,
        k_32_32_FLOAT = 37,
        k_32_32_32_32_FLOAT = 38,
        k_32_AS_8 = 39,
        k_32_AS_8_8 = 40,
        k_16_MPEG = 41,
        k_16_16_MPEG = 42,
        k_8_INTERLACED = 43,
        k_32_AS_8_INTERLACED = 44,
        k_32_AS_8_8_INTERLACED = 45,
        k_16_INTERLACED = 46,
        k_16_MPEG_INTERLACED = 47,
        k_16_16_MPEG_INTERLACED = 48,
        k_DXN = 49,
        k_8_8_8_8_AS_16_16_16_16 = 50,
        k_DXT1_AS_16_16_16_16 = 51,
        k_DXT2_3_AS_16_16_16_16 = 52,
        k_DXT4_5_AS_16_16_16_16 = 53,
        k_2_10_10_10_AS_16_16_16_16 = 54,
        k_10_11_11_AS_16_16_16_16 = 55,
        k_11_11_10_AS_16_16_16_16 = 56,
        k_32_32_32_FLOAT = 57,
        k_DXT3A = 58,
        k_DXT5A = 59,
        k_CTX1 = 60,
        k_DXT3A_AS_1_1_1_1 = 61,
        k_8_8_8_8_GAMMA_EDRAM = 62,
        k_2_10_10_10_FLOAT_EDRAM = 63,
    };

    enum class VertexFormat : uint32_t
    {
        Undefined = 0,
        k_8_8_8_8 = 6,
        k_2_10_10_10 = 7,
        k_10_11_11 = 16,
        k_11_11_10 = 17,
        k_16_16 = 25,
        k_16_16_16_16 = 26,
        k_16_16_FLOAT = 31,
        k_16_16_16_16_FLOAT = 32,
        k_32 = 33,
        k_32_32 = 34,
        k_32_32_32_32 = 35,
        k_32_FLOAT = 36,
        k_32_32_FLOAT = 37,
        k_32_32_32_32_FLOAT = 38,
        k_32_32_32_FLOAT = 57,
    };

    // Dwords one vertex element of `format` occupies.
    inline uint32_t VertexFormatDwords(uint32_t format)
    {
        switch (VertexFormat(format))
        {
        case VertexFormat::k_16_16_16_16:
        case VertexFormat::k_16_16_16_16_FLOAT:
        case VertexFormat::k_32_32:
        case VertexFormat::k_32_32_FLOAT:
            return 2;
        case VertexFormat::k_32_32_32_FLOAT:
            return 3;
        case VertexFormat::k_32_32_32_32:
        case VertexFormat::k_32_32_32_32_FLOAT:
            return 4;
        default:
            return 1;
        }
    }

    enum class CompareFunction : uint32_t
    {
        Never = 0,
        Less = 1,
        Equal = 2,
        LessEqual = 3,
        Greater = 4,
        NotEqual = 5,
        GreaterEqual = 6,
        Always = 7,
    };

    enum class BlendFactor : uint32_t
    {
        Zero = 0,
        One = 1,
        SrcColor = 4,
        OneMinusSrcColor = 5,
        SrcAlpha = 6,
        OneMinusSrcAlpha = 7,
        DstColor = 8,
        OneMinusDstColor = 9,
        DstAlpha = 10,
        OneMinusDstAlpha = 11,
        ConstantColor = 12,
        OneMinusConstantColor = 13,
        ConstantAlpha = 14,
        OneMinusConstantAlpha = 15,
        SrcAlphaSaturate = 16,
    };

    enum class BlendOp : uint32_t
    {
        Add = 0,
        Subtract = 1,
        Min = 2,
        Max = 3,
        RevSubtract = 4,
    };

    enum class CopyCommand : uint32_t
    {
        Raw = 0,
        Convert = 1,
        ConstantOne = 2,
        Null = 3,
    };

    enum class ClampMode : uint32_t
    {
        Repeat = 0,
        MirroredRepeat = 1,
        ClampToEdge = 2,
        MirrorClampToEdge = 3,
        ClampToHalfway = 4,
        MirrorClampToHalfway = 5,
        ClampToBorder = 6,
        MirrorClampToBorder = 7,
    };

    enum class TextureFilter : uint32_t
    {
        Point = 0,
        Linear = 1,
        BaseMap = 2,
        UseFetchConst = 3,
    };

    enum class TextureSign : uint32_t
    {
        Unsigned = 0,
        Signed = 1,
        UnsignedBiased = 2,
        Gamma = 3,
    };

    // Texture fetch constant (6 dwords, already in host order).
    struct TextureFetch
    {
        uint32_t dword[6];

        uint32_t Type() const { return Bits(dword[0], 0, 2); }
        uint32_t Sign(uint32_t component) const { return Bits(dword[0], 2 + component * 2, 2); }
        uint32_t ClampX() const { return Bits(dword[0], 10, 3); }
        uint32_t ClampY() const { return Bits(dword[0], 13, 3); }
        uint32_t ClampZ() const { return Bits(dword[0], 16, 3); }
        uint32_t Pitch() const { return Bits(dword[0], 22, 9) << 5; } // texels
        bool Tiled() const { return Bits(dword[0], 31, 1) != 0; }
        uint32_t Format() const { return Bits(dword[1], 0, 6); }
        uint32_t Endianness() const { return Bits(dword[1], 6, 2); }
        bool Stacked() const { return Bits(dword[1], 10, 1) != 0; }
        uint32_t BaseAddress() const { return dword[1] & 0xFFFFF000; }
        uint32_t Dimension() const { return Bits(dword[5], 9, 2); } // 0 1D, 1 2D/stacked, 2 3D, 3 cube
        uint32_t Width() const
        {
            return Dimension() == 0 ? Bits(dword[2], 0, 24) + 1 : (Dimension() == 2 ? Bits(dword[2], 0, 11) + 1 : Bits(dword[2], 0, 13) + 1);
        }
        uint32_t Height() const
        {
            return Dimension() == 0 ? 1 : (Dimension() == 2 ? Bits(dword[2], 11, 11) + 1 : Bits(dword[2], 13, 13) + 1);
        }
        uint32_t Depth() const
        {
            return Dimension() == 2 ? Bits(dword[2], 22, 10) + 1 : (Dimension() == 3 ? 6 : (Stacked() ? Bits(dword[2], 26, 6) + 1 : 1));
        }
        uint32_t NumFormat() const { return Bits(dword[3], 0, 1); }
        uint32_t Swizzle() const { return Bits(dword[3], 1, 12); }
        int32_t ExpAdjust() const { return SignedBits(dword[3], 13, 6); }
        uint32_t MagFilter() const { return Bits(dword[3], 19, 2); }
        uint32_t MinFilter() const { return Bits(dword[3], 21, 2); }
        uint32_t MipFilter() const { return Bits(dword[3], 23, 2); }
        uint32_t AnisoFilter() const { return Bits(dword[3], 25, 3); }
        uint32_t MipMinLevel() const { return Bits(dword[4], 2, 4); }
        uint32_t MipMaxLevel() const { return Bits(dword[4], 6, 4); }
        int32_t LodBias() const { return SignedBits(dword[4], 12, 10); } // 5 fractional bits
        uint32_t BorderColor() const { return Bits(dword[5], 0, 2); }
        bool PackedMips() const { return Bits(dword[5], 11, 1) != 0; }
        uint32_t MipAddress() const { return dword[5] & 0xFFFFF000; }
    };

    // Vertex fetch constant (2 dwords, host order).
    struct VertexFetch
    {
        uint32_t dword[2];

        uint32_t Type() const { return Bits(dword[0], 0, 2); }
        uint32_t Address() const { return dword[0] & 0xFFFFFFFC; } // bytes
        uint32_t Endianness() const { return Bits(dword[1], 0, 2); }
        uint32_t SizeDwords() const { return Bits(dword[1], 2, 24); }
    };

    // 2D tiled texture addressing (bytes offset of block (x, y)); see Xenia's texture_address.h.
    // `pitch` is in blocks, aligned to 32; `bppLog2` is log2(bytes per block).
    inline int32_t Tiled2D(int32_t x, int32_t y, uint32_t pitch, uint32_t bppLog2)
    {
        int32_t outerBlocks = (((y >> 5) * int32_t(pitch >> 5)) + (x >> 5)) << 6;
        int32_t innerBlocks = (((y >> 1) & 7) << 3) | (x & 7);
        int32_t outerInnerBytes = (outerBlocks | innerBlocks) << bppLog2;
        uint32_t bank = (y >> 4) & 1;
        uint32_t pipe = ((x >> 3) & 3) ^ (((y >> 3) & 1) << 1);
        return int32_t(((y & 1) << 4) | (pipe << 6) | (bank << 11)) | (outerInnerBytes & 0xF) |
               (((outerInnerBytes >> 4) & 1) << 5) | (((outerInnerBytes >> 5) & 7) << 8) | ((outerInnerBytes >> 8) << 12);
    }
}
