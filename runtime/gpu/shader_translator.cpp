// Xenos microcode -> HLSL. See shader_translator.h for the interface with the renderer.
//
// Microcode layout (Xenia ucode.h): control flow instructions (48 bits, packed in pairs into 3
// dwords) come first; exec instructions point at ALU/fetch instruction slots (3 dwords each)
// further on. Checked against the XDK disassembly in the game's shaders\*.asm files.

#include "shader_translator.h"
#include "xenos.h"

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>

namespace gpu
{
    namespace
    {
        bool g_renderScaled = false;  // SetRenderScaled

        void Append(std::string& out, const char* format, ...)
        {
            char buffer[1024];
            va_list args;
            va_start(args, format);
            int length = vsnprintf(buffer, sizeof(buffer), format, args);
            va_end(args);
            if (length > 0)
                out.append(buffer, std::min<size_t>(size_t(length), sizeof(buffer) - 1));
        }

        constexpr const char* COMPONENTS = "xyzw";

        // Control flow -------------------------------------------------------------------------

        enum CfOpcode : uint32_t
        {
            CF_NOP = 0,
            CF_EXEC = 1,
            CF_EXEC_END = 2,
            CF_COND_EXEC = 3,
            CF_COND_EXEC_END = 4,
            CF_COND_EXEC_PRED = 5,
            CF_COND_EXEC_PRED_END = 6,
            CF_LOOP_START = 7,
            CF_LOOP_END = 8,
            CF_COND_CALL = 9,
            CF_RETURN = 10,
            CF_COND_JMP = 11,
            CF_ALLOC = 12,
            CF_COND_EXEC_PRED_CLEAN = 13,
            CF_COND_EXEC_PRED_CLEAN_END = 14,
            CF_MARK_VS_FETCH_DONE = 15,
        };

        struct Cf
        {
            uint32_t dword0; // bits 0-31
            uint32_t dword1; // bits 32-47 (low 16 bits)

            uint32_t Opcode() const { return (dword1 >> 12) & 0xF; }
            bool IsExec() const
            {
                switch (Opcode())
                {
                case CF_EXEC: case CF_EXEC_END: case CF_COND_EXEC: case CF_COND_EXEC_END:
                case CF_COND_EXEC_PRED: case CF_COND_EXEC_PRED_END:
                case CF_COND_EXEC_PRED_CLEAN: case CF_COND_EXEC_PRED_CLEAN_END:
                    return true;
                default:
                    return false;
                }
            }
            bool EndsShader() const
            {
                uint32_t op = Opcode();
                return op == CF_EXEC_END || op == CF_COND_EXEC_END || op == CF_COND_EXEC_PRED_END || op == CF_COND_EXEC_PRED_CLEAN_END;
            }
            // Exec.
            uint32_t ExecAddress() const { return dword0 & 0xFFF; }
            uint32_t ExecCount() const { return (dword0 >> 12) & 7; }
            uint32_t ExecSequence() const { return (dword0 >> 16) & 0xFFF; }
            // Cond exec (bool constant) / cond call / cond jmp.
            uint32_t BoolAddress() const { return (dword1 >> 2) & 0xFF; }
            bool Condition() const { return (dword1 >> 10) & 1; }
            // Loop / call / jump.
            uint32_t TargetAddress() const { return dword0 & 0x1FFF; }
            bool LoopIsRepeat() const { return (dword0 >> 13) & 1; }
            uint32_t LoopId() const { return (dword0 >> 16) & 0x1F; }
            bool LoopEndPredicatedBreak() const { return (dword0 >> 21) & 1; }
            bool IsUnconditional() const { return (dword0 >> 13) & 1; }
            bool IsPredicated() const { return (dword0 >> 14) & 1; }
        };

        // ALU ----------------------------------------------------------------------------------

        enum VectorOpcode : uint32_t
        {
            V_ADD = 0, V_MUL = 1, V_MAX = 2, V_MIN = 3, V_SEQ = 4, V_SGT = 5, V_SGE = 6, V_SNE = 7,
            V_FRC = 8, V_TRUNC = 9, V_FLOOR = 10, V_MAD = 11, V_CNDEQ = 12, V_CNDGE = 13, V_CNDGT = 14,
            V_DP4 = 15, V_DP3 = 16, V_DP2ADD = 17, V_CUBE = 18, V_MAX4 = 19,
            V_SETP_EQ_PUSH = 20, V_SETP_NE_PUSH = 21, V_SETP_GT_PUSH = 22, V_SETP_GE_PUSH = 23,
            V_KILL_EQ = 24, V_KILL_GT = 25, V_KILL_GE = 26, V_KILL_NE = 27, V_DST = 28, V_MAXA = 29,
        };

        enum ScalarOpcode : uint32_t
        {
            S_ADDS = 0, S_ADDS_PREV = 1, S_MULS = 2, S_MULS_PREV = 3, S_MULS_PREV2 = 4, S_MAXS = 5,
            S_MINS = 6, S_SEQS = 7, S_SGTS = 8, S_SGES = 9, S_SNES = 10, S_FRCS = 11, S_TRUNCS = 12,
            S_FLOORS = 13, S_EXP = 14, S_LOGC = 15, S_LOG = 16, S_RCPC = 17, S_RCPF = 18, S_RCP = 19,
            S_RSQC = 20, S_RSQF = 21, S_RSQ = 22, S_MAXAS = 23, S_MAXASF = 24, S_SUBS = 25,
            S_SUBS_PREV = 26, S_SETP_EQ = 27, S_SETP_NE = 28, S_SETP_GT = 29, S_SETP_GE = 30,
            S_SETP_INV = 31, S_SETP_POP = 32, S_SETP_CLR = 33, S_SETP_RSTR = 34, S_KILLS_EQ = 35,
            S_KILLS_GT = 36, S_KILLS_GE = 37, S_KILLS_NE = 38, S_KILLS_ONE = 39, S_SQRT = 40,
            S_MULSC0 = 42, S_MULSC1 = 43, S_ADDSC0 = 44, S_ADDSC1 = 45, S_SUBSC0 = 46, S_SUBSC1 = 47,
            S_SIN = 48, S_COS = 49, S_RETAIN_PREV = 50,
        };

        // Number of vector operands (src1..src3) each vector opcode reads.
        uint32_t VectorOperandCount(uint32_t op)
        {
            switch (op)
            {
            case V_FRC: case V_TRUNC: case V_FLOOR: case V_MAX4: case V_CUBE:
                return 1;
            case V_MAD: case V_CNDEQ: case V_CNDGE: case V_CNDGT: case V_DP2ADD:
                return 3;
            default:
                return 2;
            }
        }

        // Scalar operand form: 0 none, 1 = src3.a, 2 = src3.ab, 3 = constant.a + temp.b.
        uint32_t ScalarOperandForm(uint32_t op)
        {
            switch (op)
            {
            case S_ADDS: case S_MULS: case S_MULS_PREV2: case S_MAXS: case S_MINS: case S_MAXAS:
            case S_MAXASF: case S_SUBS:
                return 2;
            case S_SETP_CLR: case S_RETAIN_PREV:
                return 0;
            case S_MULSC0: case S_MULSC1: case S_ADDSC0: case S_ADDSC1: case S_SUBSC0: case S_SUBSC1:
                return 3;
            default:
                return 1;
            }
        }

        struct Alu
        {
            uint32_t d0, d1, d2;

            uint32_t VectorDest() const { return d0 & 0x3F; }
            bool VectorDestRelative() const { return (d0 >> 6) & 1; }
            bool AbsConstants() const { return (d0 >> 7) & 1; }
            uint32_t ScalarDest() const { return (d0 >> 8) & 0x3F; }
            bool ScalarDestRelative() const { return (d0 >> 14) & 1; }
            bool IsExport() const { return (d0 >> 15) & 1; }
            uint32_t VectorMask() const { return (d0 >> 16) & 0xF; }
            uint32_t ScalarMask() const { return (d0 >> 20) & 0xF; }
            bool VectorClamp() const { return (d0 >> 24) & 1; }
            bool ScalarClamp() const { return (d0 >> 25) & 1; }
            uint32_t ScalarOp() const { return (d0 >> 26) & 0x3F; }
            uint32_t SrcSwizzle(uint32_t i) const { return (d1 >> (8 * (3 - i))) & 0xFF; }
            bool SrcNegate(uint32_t i) const { return (d1 >> (24 + 3 - i)) & 1; }
            bool PredCondition() const { return (d1 >> 27) & 1; }
            bool IsPredicated() const { return (d1 >> 28) & 1; }
            bool ConstAddressRegisterRelative() const { return (d1 >> 29) & 1; }
            bool Const1RelAbs() const { return (d1 >> 30) & 1; }
            bool Const0RelAbs() const { return (d1 >> 31) & 1; }
            uint32_t SrcReg(uint32_t i) const { return (d2 >> (8 * (3 - i))) & 0xFF; }
            uint32_t VectorOp() const { return (d2 >> 24) & 0x1F; }
            bool SrcIsTemp(uint32_t i) const { return (d2 >> (29 + 3 - i)) & 1; }

            // Which of const_0/const_1 addressing bits applies to constant operand i (Xenia).
            bool SrcConstIsAddressed(uint32_t i) const
            {
                switch (i)
                {
                case 1: return Const0RelAbs();
                case 2: return SrcIsTemp(1) ? Const0RelAbs() : Const1RelAbs();
                default: return (SrcIsTemp(1) && SrcIsTemp(2)) ? Const0RelAbs() : Const1RelAbs();
                }
            }
        };

        // Absolute component selected by a relative ALU swizzle for result component `i`.
        uint32_t SwizzledComponent(uint32_t swizzle, uint32_t i)
        {
            return ((swizzle >> (2 * i)) + i) & 3;
        }

        // Fetch --------------------------------------------------------------------------------

        enum FetchOpcode : uint32_t
        {
            F_VERTEX = 0,
            F_TEXTURE = 1,
            F_GET_BORDER_COLOR_FRAC = 16,
            F_GET_COMPUTED_LOD = 17,
            F_GET_GRADIENTS = 18,
            F_GET_WEIGHTS = 19,
            F_SET_LOD = 24,
            F_SET_GRADIENTS_H = 25,
            F_SET_GRADIENTS_V = 26,
        };

        struct Fetch
        {
            uint32_t d0, d1, d2;

            uint32_t Opcode() const { return d0 & 0x1F; }
            uint32_t SrcReg() const { return (d0 >> 5) & 0x3F; }
            bool SrcRelative() const { return (d0 >> 11) & 1; }
            uint32_t DstReg() const { return (d0 >> 12) & 0x3F; }
            bool DstRelative() const { return (d0 >> 18) & 1; }
            uint32_t DstSwizzle() const { return d1 & 0xFFF; }
            bool IsPredicated() const { return (d1 >> 31) & 1; }
            bool PredCondition() const { return (d2 >> 31) & 1; }

            // Vertex fetch.
            uint32_t VertexFetchConstant() const { return ((d0 >> 20) & 0x1F) * 3 + ((d0 >> 25) & 3); }
            uint32_t VertexSrcComponent() const { return (d0 >> 30) & 3; }
            bool VertexSigned() const { return (d1 >> 12) & 1; }
            bool VertexInteger() const { return (d1 >> 13) & 1; } // num_format_all: 0 = normalized
            bool VertexIndexRounded() const { return (d1 >> 15) & 1; }
            uint32_t VertexFormat() const { return (d1 >> 16) & 0x3F; }
            int32_t VertexExpAdjust() const { return xenos::SignedBits(d1, 24, 6); }
            bool VertexIsMini() const { return (d1 >> 30) & 1; }
            uint32_t VertexStride() const { return d2 & 0xFF; }
            int32_t VertexOffset() const { return xenos::SignedBits(d2, 8, 23); }

            // Texture fetch.
            uint32_t TextureFetchConstant() const { return (d0 >> 20) & 0x1F; }
            bool TextureUnnormalized() const { return (d0 >> 25) & 1; }
            uint32_t TextureSrcSwizzle() const { return (d0 >> 26) & 0x3F; }
            uint32_t TextureMagFilter() const { return (d1 >> 12) & 3; }
            uint32_t TextureMinFilter() const { return (d1 >> 14) & 3; }
            uint32_t TextureMipFilter() const { return (d1 >> 16) & 3; }
            uint32_t TextureAnisoFilter() const { return (d1 >> 18) & 7; }
            bool TextureUseComputedLod() const { return (d1 >> 28) & 1; }
            bool TextureUseRegisterLod() const { return (d1 >> 29) & 1; }
            int32_t TextureLodBias() const { return xenos::SignedBits(d2, 2, 7); } // 1/16
            uint32_t TextureDimension() const { return (d2 >> 14) & 3; }
            int32_t TextureOffset(uint32_t i) const { return xenos::SignedBits(d2, 16 + 5 * i, 5); } // 1/2
        };

        // Translator ---------------------------------------------------------------------------

        class Translator
        {
        public:
            Translator(ShaderStage stage, const uint32_t* ucode, uint32_t dwordCount, bool renderScaled)
                : m_stage(stage), m_ucode(ucode), m_dwordCount(dwordCount), m_renderScaled(renderScaled)
            {
            }

            TranslatedShader Run();

        private:
            ShaderStage m_stage;
            const uint32_t* m_ucode;
            uint32_t m_dwordCount;
            bool m_renderScaled;  // the scaled variant (SetRenderScaled)
            TranslatedShader m_out;
            std::vector<Cf> m_cf;
            std::string m_body;
            int m_indent = 1;
            uint32_t m_maxRegister = 0;
            bool m_relativeTemps = false;
            bool m_r0Dirty = false;
            // vfetch_mini reuses the address of the last vfetch_full.
            uint32_t m_lastFullFetchConstant = 0;
            bool m_haveFullFetch = false;
            // Pixel shader texture fetches sampled with gradients (by instruction slot): their
            // coordinates are still the unmodified input register, whose derivatives main()
            // computes before running the shader (m_gradientRegisters, bit per register).
            std::vector<uint8_t> m_gradientFetch;
            uint32_t m_gradientRegisters = 0;
            uint32_t m_currentSlot = 0;

            bool IsPixel() const { return m_stage == ShaderStage::Pixel; }

            void Line(const char* format, ...);
            void ParseControlFlow();
            void ScanRegisters();
            void FindGradientFetches();
            void EmitStructured();
            void EmitStateMachine();
            void EmitExecBody(const Cf& cf);
            std::string ExecCondition(const Cf& cf) const;
            std::string BoolConstant(uint32_t index) const;

            void EmitAlu(const Alu& alu);
            void EmitFetch(const Fetch& fetch);
            void EmitVertexFetch(const Fetch& fetch);
            void EmitTextureFetch(const Fetch& fetch);

            std::string TempRegister(uint32_t index, bool relative) const;
            std::string AluSource(const Alu& alu, uint32_t i) const;      // full float4 with swizzle/abs/negate
            // forceConstant: read src i as a float constant whatever its sel bit says (the
            // constant operand of MULSC/ADDSC/SUBSC, whose src3 sel bit is part of the temp index).
            std::string AluSourceRaw(const Alu& alu, uint32_t i, bool& absolute, bool forceConstant = false) const;
            void WriteFetchResult(const Fetch& fetch, const char* value);
            void NoteTempWrite(uint32_t reg, bool relative)
            {
                if (relative || reg == 0)
                    m_r0Dirty = true;
            }

            VertexFetchBinding& VertexBinding(uint32_t fetchConstant);
            TextureBinding& TextureBindingFor(uint32_t fetchConstant);
        };

        void Translator::Line(const char* format, ...)
        {
            m_body.append(size_t(m_indent) * 4, ' ');
            char buffer[2048];
            va_list args;
            va_start(args, format);
            int length = vsnprintf(buffer, sizeof(buffer), format, args);
            va_end(args);
            if (length > 0)
                m_body.append(buffer, std::min<size_t>(size_t(length), sizeof(buffer) - 1));
            m_body.push_back('\n');
        }

        void Translator::ParseControlFlow()
        {
            // Control flow ends where the first exec's instructions begin (3 dwords per slot).
            uint32_t maxDword = m_dwordCount;
            for (uint32_t i = 0; i + 3 <= maxDword; i += 3)
            {
                Cf pair[2];
                pair[0].dword0 = m_ucode[i];
                pair[0].dword1 = m_ucode[i + 1] & 0xFFFF;
                pair[1].dword0 = (m_ucode[i + 1] >> 16) | (m_ucode[i + 2] << 16);
                pair[1].dword1 = m_ucode[i + 2] >> 16;
                for (const Cf& cf : pair)
                {
                    if (cf.IsExec())
                        maxDword = std::min(maxDword, cf.ExecAddress() * 3);
                    m_cf.push_back(cf);
                    uint32_t op = cf.Opcode();
                    if (op == CF_LOOP_START || op == CF_LOOP_END || op == CF_COND_CALL || op == CF_RETURN || op == CF_COND_JMP)
                        m_out.usesControlFlowMachine = true;
                }
            }
        }

        // Finds the highest temp register index and whether relative addressing is used.
        void Translator::ScanRegisters()
        {
            auto note = [&](uint32_t reg) { m_maxRegister = std::max(m_maxRegister, reg); };
            for (const Cf& cf : m_cf)
            {
                if (!cf.IsExec())
                    continue;
                for (uint32_t i = 0; i < cf.ExecCount(); i++)
                {
                    uint32_t slot = cf.ExecAddress() + i;
                    if (slot * 3 + 3 > m_dwordCount)
                        break;
                    const uint32_t* words = m_ucode + slot * 3;
                    if ((cf.ExecSequence() >> (i * 2)) & 1)
                    {
                        Fetch fetch{ words[0], words[1], words[2] };
                        note(fetch.SrcReg());
                        note(fetch.DstReg());
                        m_relativeTemps |= fetch.SrcRelative() || fetch.DstRelative();
                    }
                    else
                    {
                        Alu alu{ words[0], words[1], words[2] };
                        if (!alu.IsExport())
                        {
                            note(alu.VectorDest());
                            note(alu.ScalarDest());
                            m_relativeTemps |= alu.VectorDestRelative() || alu.ScalarDestRelative();
                        }
                        for (uint32_t s = 1; s <= 3; s++)
                        {
                            if (alu.SrcIsTemp(s))
                            {
                                note(alu.SrcReg(s) & 0x3F);
                                m_relativeTemps |= (alu.SrcReg(s) & 0x40) != 0;
                            }
                        }
                        uint32_t form = ScalarOperandForm(alu.ScalarOp());
                        if (form == 3)
                            note((alu.ScalarOp() & 1) | (uint32_t(alu.SrcIsTemp(3)) << 1) | (alu.SrcSwizzle(3) & 0x3C));
                    }
                }
            }
        }

        // Mipmapped sampling needs derivatives, which HLSL can't take inside the control flow the
        // translated shaders run in (the state machine loop). The derivative of a coordinate that
        // is still exactly a pixel shader input (interpolator or param gen) can be taken at the top
        // of main() instead. Finds the 2D fetches with computed LOD whose coordinate components
        // are not written by any instruction that can run before them (earlier in control flow
        // order, or anywhere in a loop / backward jump range containing them).
        void Translator::FindGradientFetches()
        {
            m_gradientFetch.assign(m_dwordCount / 3 + 1, 0);
            if (!IsPixel())
                return;
            struct Entry
            {
                uint32_t slot;
                uint32_t cf;
                bool isFetch;
            };
            std::vector<Entry> order;
            std::vector<std::pair<uint32_t, uint32_t>> repeats;
            for (uint32_t c = 0; c < uint32_t(m_cf.size()); c++)
            {
                const Cf& cf = m_cf[c];
                uint32_t op = cf.Opcode();
                if (op == CF_COND_CALL || op == CF_RETURN)
                    return;
                if ((op == CF_LOOP_END || op == CF_COND_JMP) && cf.TargetAddress() <= c)
                    repeats.push_back({ cf.TargetAddress(), c });
                if (!cf.IsExec())
                    continue;
                for (uint32_t i = 0; i < cf.ExecCount(); i++)
                {
                    uint32_t slot = cf.ExecAddress() + i;
                    if (slot * 3 + 3 > m_dwordCount)
                        break;
                    bool isFetch = ((cf.ExecSequence() >> (i * 2)) & 1) != 0;
                    const uint32_t* words = m_ucode + slot * 3;
                    // A relative write (r[aL + n]) can change any register: give up.
                    if (isFetch ? Fetch{ words[0], words[1], words[2] }.DstRelative()
                                : (!Alu{ words[0], words[1], words[2] }.IsExport() &&
                                   (Alu{ words[0], words[1], words[2] }.VectorDestRelative() || Alu{ words[0], words[1], words[2] }.ScalarDestRelative())))
                        return;
                    order.push_back({ slot, c, isFetch });
                }
            }
            // Components of `reg` an instruction writes.
            auto writes = [&](const Entry& entry, uint32_t reg) -> uint32_t {
                const uint32_t* words = m_ucode + entry.slot * 3;
                if (entry.isFetch)
                {
                    Fetch fetch{ words[0], words[1], words[2] };
                    uint32_t op = fetch.Opcode();
                    if (fetch.DstReg() != reg || op == F_SET_LOD || op == F_SET_GRADIENTS_H || op == F_SET_GRADIENTS_V)
                        return 0;
                    uint32_t mask = 0;
                    for (uint32_t i = 0; i < 4; i++)
                        mask |= (((fetch.DstSwizzle() >> (3 * i)) & 7) != 7 ? 1u : 0u) << i;
                    return mask;
                }
                Alu alu{ words[0], words[1], words[2] };
                if (alu.IsExport())
                    return 0;
                uint32_t mask = 0;
                if (alu.VectorDest() == reg)
                    mask |= alu.VectorMask();
                if (alu.ScalarDest() == reg)
                    mask |= alu.ScalarMask();
                return mask;
            };
            for (size_t k = 0; k < order.size(); k++)
            {
                if (!order[k].isFetch)
                    continue;
                const uint32_t* words = m_ucode + order[k].slot * 3;
                Fetch fetch{ words[0], words[1], words[2] };
                if (fetch.Opcode() != F_TEXTURE || fetch.TextureDimension() != 1 || !fetch.TextureUseComputedLod() ||
                    fetch.TextureUseRegisterLod() || fetch.SrcRelative() || fetch.SrcReg() >= 16)
                    continue;
                uint32_t reg = fetch.SrcReg();
                uint32_t used = 0;
                for (uint32_t c = 0; c < 2; c++)
                    used |= 1u << ((fetch.TextureSrcSwizzle() >> (2 * c)) & 3);
                bool pristine = true;
                for (size_t j = 0; j < k && pristine; j++)
                    pristine = !(writes(order[j], reg) & used);
                for (const auto& [first, last] : repeats)
                {
                    if (!pristine || order[k].cf < first || order[k].cf > last)
                        continue;
                    for (const Entry& entry : order)
                    {
                        if (entry.cf >= first && entry.cf <= last && (writes(entry, reg) & used))
                            pristine = false;
                    }
                }
                if (pristine)
                {
                    m_gradientFetch[order[k].slot] = 1;
                    m_gradientRegisters |= 1u << reg;
                }
            }
        }

        std::string Translator::BoolConstant(uint32_t index) const
        {
            char buffer[96];
            uint32_t dword = index >> 5;
            snprintf(buffer, sizeof(buffer), "((xe_bool_consts[%u].%c >> %uu) & 1u)", dword >> 2, COMPONENTS[dword & 3], index & 31);
            return buffer;
        }

        std::string Translator::ExecCondition(const Cf& cf) const
        {
            switch (cf.Opcode())
            {
            case CF_COND_EXEC:
            case CF_COND_EXEC_END:
            case CF_COND_EXEC_PRED_CLEAN:
            case CF_COND_EXEC_PRED_CLEAN_END:
                return BoolConstant(cf.BoolAddress()) + (cf.Condition() ? " != 0u" : " == 0u");
            case CF_COND_EXEC_PRED:
            case CF_COND_EXEC_PRED_END:
                return cf.Condition() ? "xe_p0" : "!xe_p0";
            default:
                return "";
            }
        }

        std::string Translator::TempRegister(uint32_t index, bool relative) const
        {
            char buffer[64];
            if (relative)
                snprintf(buffer, sizeof(buffer), "r[clamp(xe_aL + %u, 0, 63)]", index);
            else
                snprintf(buffer, sizeof(buffer), "r[%u]", index);
            return buffer;
        }

        std::string Translator::AluSourceRaw(const Alu& alu, uint32_t i, bool& absolute, bool forceConstant) const
        {
            uint32_t reg = alu.SrcReg(i);
            char buffer[96];
            if (alu.SrcIsTemp(i) && !forceConstant)
            {
                absolute = (reg & 0x80) != 0;
                return TempRegister(reg & 0x3F, (reg & 0x40) != 0);
            }
            absolute = alu.AbsConstants();
            if (alu.SrcConstIsAddressed(i))
                snprintf(buffer, sizeof(buffer), "xe_c[clamp(int(%u) + %s, 0, 255)]", reg, alu.ConstAddressRegisterRelative() ? "xe_a0" : "xe_aL");
            else
                snprintf(buffer, sizeof(buffer), "xe_c[%u]", reg);
            return buffer;
        }

        std::string Translator::AluSource(const Alu& alu, uint32_t i) const
        {
            bool absolute;
            std::string value = AluSourceRaw(alu, i, absolute);
            if (absolute)
                value = "abs(" + value + ")";
            uint32_t swizzle = alu.SrcSwizzle(i);
            value += '.';
            for (uint32_t c = 0; c < 4; c++)
                value += COMPONENTS[SwizzledComponent(swizzle, c)];
            if (alu.SrcNegate(i))
                value = "(-" + value + ")";
            return value;
        }

        VertexFetchBinding& Translator::VertexBinding(uint32_t fetchConstant)
        {
            for (auto& binding : m_out.vertexFetches)
            {
                if (binding.fetchConstant == fetchConstant)
                    return binding;
            }
            VertexFetchBinding binding;
            binding.fetchConstant = fetchConstant;
            binding.minOffsetDwords = INT32_MAX;
            binding.maxEndDwords = INT32_MIN;
            m_out.vertexFetches.push_back(binding);
            return m_out.vertexFetches.back();
        }

        TextureBinding& Translator::TextureBindingFor(uint32_t fetchConstant)
        {
            for (auto& binding : m_out.textures)
            {
                if (binding.fetchConstant == fetchConstant)
                    return binding;
            }
            TextureBinding binding;
            binding.fetchConstant = fetchConstant;
            m_out.textures.push_back(binding);
            return m_out.textures.back();
        }

        void Translator::EmitAlu(const Alu& alu)
        {
            uint32_t vectorOp = alu.VectorOp();
            uint32_t scalarOp = alu.ScalarOp();
            bool isExport = alu.IsExport();
            uint32_t vectorMask = alu.VectorMask();
            uint32_t scalarMask = alu.ScalarMask();
            uint32_t vectorResultMask = isExport ? (vectorMask & ~scalarMask) : vectorMask;
            uint32_t scalarResultMask = isExport ? (scalarMask & ~vectorMask) : scalarMask;
            uint32_t constant1Mask = isExport ? (vectorMask & scalarMask) : 0;
            uint32_t constant0Mask = (isExport && alu.ScalarDestRelative()) ? (0xF & ~(vectorMask | scalarMask)) : 0;

            bool vectorChangesState = vectorOp == V_MAXA || (vectorOp >= V_SETP_EQ_PUSH && vectorOp <= V_KILL_NE);
            bool needVector = vectorResultMask || vectorChangesState;

            if (alu.IsPredicated())
            {
                Line("if (%sxe_p0) {", alu.PredCondition() ? "" : "!");
                m_indent++;
            }
            else
            {
                Line("{");
                m_indent++;
            }

            // Sources are read before any result is written (co-issued vector + scalar).
            std::string src[4];
            if (needVector)
            {
                uint32_t count = VectorOperandCount(vectorOp);
                for (uint32_t i = 1; i <= count; i++)
                {
                    src[i] = AluSource(alu, i);
                    Line("float4 s%u = %s;", i, src[i].c_str());
                }
            }

            // Scalar operands.
            uint32_t form = ScalarOperandForm(scalarOp);
            if (form == 1 || form == 2)
            {
                bool absolute;
                std::string raw = AluSourceRaw(alu, 3, absolute);
                uint32_t swizzle = alu.SrcSwizzle(3);
                const char* neg = alu.SrcNegate(3) ? "-" : "";
                Line("float sa = %s%s%s.%c%s;", neg, absolute ? "abs(" : "", raw.c_str(), COMPONENTS[SwizzledComponent(swizzle, 3)], absolute ? ")" : "");
                if (form == 2)
                    Line("float sb = %s%s%s.%c%s;", neg, absolute ? "abs(" : "", raw.c_str(), COMPONENTS[SwizzledComponent(swizzle, 0)], absolute ? ")" : "");
            }
            else if (form == 3)
            {
                // Constant (src3, a = W swizzle) and temp register built from the opcode/sel/swizzle bits.
                bool unused;
                std::string constant = AluSourceRaw(alu, 3, unused, true);
                uint32_t swizzle = alu.SrcSwizzle(3);
                uint32_t temp = (scalarOp & 1) | (uint32_t(alu.SrcIsTemp(3)) << 1) | (swizzle & 0x3C);
                const char* neg = alu.SrcNegate(3) ? "-" : "";
                bool absolute = alu.AbsConstants();
                Line("float sa = %s%s%s.%c%s;", neg, absolute ? "abs(" : "", constant.c_str(), COMPONENTS[SwizzledComponent(swizzle, 3)], absolute ? ")" : "");
                Line("float sb = %s%sr[%u].%c%s;", neg, absolute ? "abs(" : "", temp, COMPONENTS[SwizzledComponent(swizzle, 0)], absolute ? ")" : "");
            }

            // Vector operation.
            if (needVector)
            {
                switch (vectorOp)
                {
                case V_ADD: Line("float4 v = s1 + s2;"); break;
                case V_MUL: Line("float4 v = xe_mul(s1, s2);"); break;
                case V_MAX: Line("float4 v = xe_max(s1, s2);"); break;
                case V_MIN: Line("float4 v = xe_min(s1, s2);"); break;
                case V_SEQ: Line("float4 v = float4(s1 == s2);"); break;
                case V_SGT: Line("float4 v = float4(s1 > s2);"); break;
                case V_SGE: Line("float4 v = float4(s1 >= s2);"); break;
                case V_SNE: Line("float4 v = float4(s1 != s2);"); break;
                case V_FRC: Line("float4 v = s1 - floor(s1);"); break;
                case V_TRUNC: Line("float4 v = trunc(s1);"); break;
                case V_FLOOR: Line("float4 v = floor(s1);"); break;
                case V_MAD: Line("float4 v = xe_mul(s1, s2) + s3;"); break;
                case V_CNDEQ: Line("float4 v = (s1 == 0.0) ? s2 : s3;"); break;
                case V_CNDGE: Line("float4 v = (s1 >= 0.0) ? s2 : s3;"); break;
                case V_CNDGT: Line("float4 v = (s1 > 0.0) ? s2 : s3;"); break;
                case V_DP4: Line("float4 v = xe_dp4(s1, s2).xxxx;"); break;
                case V_DP3: Line("float4 v = xe_dp3(s1, s2).xxxx;"); break;
                case V_DP2ADD: Line("float4 v = (xe_dp2(s1, s2) + s3.x).xxxx;"); break;
                case V_CUBE: Line("float4 v = xe_cube(s1);"); break;
                case V_MAX4: Line("float4 v = xe_max4(s1).xxxx;"); break;
                case V_SETP_EQ_PUSH:
                    Line("xe_p0 = s1.w == 0.0 && s2.w == 0.0;");
                    Line("float4 v = ((s1.x == 0.0 && s2.x == 0.0) ? 0.0 : s1.x + 1.0).xxxx;");
                    break;
                case V_SETP_NE_PUSH:
                    Line("xe_p0 = s1.w == 0.0 && s2.w != 0.0;");
                    Line("float4 v = ((s1.x == 0.0 && s2.x != 0.0) ? 0.0 : s1.x + 1.0).xxxx;");
                    break;
                case V_SETP_GT_PUSH:
                    Line("xe_p0 = s1.w == 0.0 && s2.w > 0.0;");
                    Line("float4 v = ((s1.x == 0.0 && s2.x > 0.0) ? 0.0 : s1.x + 1.0).xxxx;");
                    break;
                case V_SETP_GE_PUSH:
                    Line("xe_p0 = s1.w == 0.0 && s2.w >= 0.0;");
                    Line("float4 v = ((s1.x == 0.0 && s2.x >= 0.0) ? 0.0 : s1.x + 1.0).xxxx;");
                    break;
                case V_KILL_EQ: case V_KILL_GT: case V_KILL_GE: case V_KILL_NE:
                {
                    const char* op = vectorOp == V_KILL_EQ ? "==" : vectorOp == V_KILL_GT ? ">" : vectorOp == V_KILL_GE ? ">=" : "!=";
                    Line("float4 v = float(any(s1 %s s2)).xxxx;", op);
                    if (IsPixel())
                    {
                        Line("if (v.x != 0.0) discard;");
                        m_out.hasKill = true;
                    }
                    break;
                }
                case V_DST: Line("float4 v = float4(1.0, xe_mul(s1, s2).y, s1.z, s2.w);"); break;
                case V_MAXA:
                    Line("xe_a0 = int(floor(clamp(s1.w, -256.0, 255.0) + 0.5));");
                    Line("float4 v = xe_max(s1, s2);");
                    break;
                default:
                    Line("float4 v = 0.0; // unknown vector opcode %u", vectorOp);
                    break;
                }
                if (alu.VectorClamp())
                    Line("v = saturate(v);");
            }

            // Scalar operation (updates ps).
            bool scalarWritesPs = true;
            switch (scalarOp)
            {
            case S_ADDS: case S_ADDSC0: case S_ADDSC1: Line("xe_ps = sa + sb;"); break;
            case S_ADDS_PREV: Line("xe_ps = sa + xe_ps;"); break;
            case S_MULS: case S_MULSC0: case S_MULSC1: Line("xe_ps = xe_muls(sa, sb);"); break;
            case S_MULS_PREV: Line("xe_ps = xe_muls(sa, xe_ps);"); break;
            case S_MULS_PREV2:
                Line("xe_ps = (xe_ps == -3.402823466e+38 || !isfinite(xe_ps) || !isfinite(sb) || sb <= 0.0) ? -3.402823466e+38 : xe_muls(sa, xe_ps);");
                break;
            case S_MAXS: Line("xe_ps = sa >= sb ? sa : sb;"); break;
            case S_MINS: Line("xe_ps = sa < sb ? sa : sb;"); break;
            case S_SEQS: Line("xe_ps = float(sa == 0.0);"); break;
            case S_SGTS: Line("xe_ps = float(sa > 0.0);"); break;
            case S_SGES: Line("xe_ps = float(sa >= 0.0);"); break;
            case S_SNES: Line("xe_ps = float(sa != 0.0);"); break;
            case S_FRCS: Line("xe_ps = sa - floor(sa);"); break;
            case S_TRUNCS: Line("xe_ps = trunc(sa);"); break;
            case S_FLOORS: Line("xe_ps = floor(sa);"); break;
            case S_EXP: Line("xe_ps = exp2(sa);"); break;
            case S_LOGC: Line("xe_ps = log2(sa); if (isinf(xe_ps) && xe_ps < 0.0) xe_ps = -3.402823466e+38;"); break;
            case S_LOG: Line("xe_ps = log2(sa);"); break;
            case S_RCPC: Line("xe_ps = clamp(1.0 / sa, -3.402823466e+38, 3.402823466e+38);"); break;
            case S_RCPF: Line("xe_ps = 1.0 / sa; if (isinf(xe_ps)) xe_ps = 0.0;"); break;
            case S_RCP: Line("xe_ps = 1.0 / sa;"); break;
            case S_RSQC: Line("xe_ps = clamp(rsqrt(sa), -3.402823466e+38, 3.402823466e+38);"); break;
            case S_RSQF: Line("xe_ps = rsqrt(sa); if (isinf(xe_ps)) xe_ps = 0.0;"); break;
            case S_RSQ: Line("xe_ps = rsqrt(sa);"); break;
            case S_MAXAS:
                Line("xe_a0 = int(floor(clamp(sa, -256.0, 255.0) + 0.5));");
                Line("xe_ps = sa >= sb ? sa : sb;");
                break;
            case S_MAXASF:
                Line("xe_a0 = int(floor(clamp(sa, -256.0, 255.0)));");
                Line("xe_ps = sa >= sb ? sa : sb;");
                break;
            case S_SUBS: case S_SUBSC0: case S_SUBSC1: Line("xe_ps = sa - sb;"); break;
            case S_SUBS_PREV: Line("xe_ps = sa - xe_ps;"); break;
            case S_SETP_EQ: Line("xe_p0 = sa == 0.0; xe_ps = xe_p0 ? 0.0 : 1.0;"); break;
            case S_SETP_NE: Line("xe_p0 = sa != 0.0; xe_ps = xe_p0 ? 0.0 : 1.0;"); break;
            case S_SETP_GT: Line("xe_p0 = sa > 0.0; xe_ps = xe_p0 ? 0.0 : 1.0;"); break;
            case S_SETP_GE: Line("xe_p0 = sa >= 0.0; xe_ps = xe_p0 ? 0.0 : 1.0;"); break;
            case S_SETP_INV: Line("xe_p0 = sa == 1.0; xe_ps = xe_p0 ? 0.0 : (sa == 0.0 ? 1.0 : sa);"); break;
            case S_SETP_POP: Line("xe_p0 = (sa - 1.0) <= 0.0; xe_ps = xe_p0 ? 0.0 : sa - 1.0;"); break;
            case S_SETP_CLR: Line("xe_p0 = false; xe_ps = 3.402823466e+38;"); break;
            case S_SETP_RSTR: Line("xe_p0 = sa == 0.0; xe_ps = xe_p0 ? 0.0 : sa;"); break;
            case S_KILLS_EQ: case S_KILLS_GT: case S_KILLS_GE: case S_KILLS_NE: case S_KILLS_ONE:
            {
                const char* condition = scalarOp == S_KILLS_EQ ? "sa == 0.0" : scalarOp == S_KILLS_GT ? "sa > 0.0" :
                                        scalarOp == S_KILLS_GE ? "sa >= 0.0" : scalarOp == S_KILLS_NE ? "sa != 0.0" : "sa == 1.0";
                Line("xe_ps = float(%s);", condition);
                if (IsPixel())
                {
                    Line("if (xe_ps != 0.0) discard;");
                    m_out.hasKill = true;
                }
                break;
            }
            case S_SQRT: Line("xe_ps = sqrt(sa);"); break;
            case S_SIN: Line("xe_ps = sin(sa);"); break;
            case S_COS: Line("xe_ps = cos(sa);"); break;
            case S_RETAIN_PREV: scalarWritesPs = false; break;
            default:
                Line("// unknown scalar opcode %u", scalarOp);
                scalarWritesPs = false;
                break;
            }
            (void)scalarWritesPs;
            bool needScalarValue = scalarResultMask != 0;
            if (needScalarValue)
                Line("float s = %s;", alu.ScalarClamp() ? "saturate(xe_ps)" : "xe_ps");

            // Results.
            auto maskString = [](uint32_t mask) {
                std::string result;
                for (uint32_t c = 0; c < 4; c++)
                {
                    if (mask & (1u << c))
                        result += COMPONENTS[c];
                }
                return result;
            };

            if (isExport)
            {
                uint32_t target = alu.VectorDest();
                std::string destination;
                if (!IsPixel())
                {
                    if (target < 16)
                        destination = "xe_o[" + std::to_string(target) + "]";
                    else if (target == 62)
                        destination = "xe_o_pos";
                    else if (target == 63)
                        destination = "xe_o_misc";
                    else if (target >= 32 && target <= 37)
                        m_out.hasMemExport = true;
                }
                else
                {
                    if (target < 4)
                    {
                        destination = "xe_oC[" + std::to_string(target) + "]";
                        m_out.colorTargetsWritten |= 1u << target;
                    }
                    else if (target == 61)
                    {
                        destination = "xe_oDepth";
                        m_out.writesDepth = true;
                    }
                    else if (target >= 32 && target <= 37)
                        m_out.hasMemExport = true;
                }

                if (!destination.empty())
                {
                    if (vectorResultMask)
                    {
                        std::string m = maskString(vectorResultMask);
                        Line("%s.%s = v.%s;", destination.c_str(), m.c_str(), m.c_str());
                    }
                    if (scalarResultMask)
                        Line("%s.%s = s;", destination.c_str(), maskString(scalarResultMask).c_str());
                    if (constant1Mask)
                        Line("%s.%s = 1.0;", destination.c_str(), maskString(constant1Mask).c_str());
                    if (constant0Mask)
                        Line("%s.%s = 0.0;", destination.c_str(), maskString(constant0Mask).c_str());
                }
            }
            else
            {
                if (vectorResultMask)
                {
                    std::string m = maskString(vectorResultMask);
                    Line("%s.%s = v.%s;", TempRegister(alu.VectorDest(), alu.VectorDestRelative()).c_str(), m.c_str(), m.c_str());
                    NoteTempWrite(alu.VectorDest(), alu.VectorDestRelative());
                }
                if (scalarResultMask)
                {
                    Line("%s.%s = s;", TempRegister(alu.ScalarDest(), alu.ScalarDestRelative()).c_str(), maskString(scalarResultMask).c_str());
                    NoteTempWrite(alu.ScalarDest(), alu.ScalarDestRelative());
                }
            }

            m_indent--;
            Line("}");
        }

        void Translator::WriteFetchResult(const Fetch& fetch, const char* value)
        {
            std::string destination = TempRegister(fetch.DstReg(), fetch.DstRelative());
            uint32_t swizzle = fetch.DstSwizzle();
            for (uint32_t c = 0; c < 4; c++)
            {
                uint32_t select = (swizzle >> (3 * c)) & 7;
                if (select == 7)
                    continue;
                if (select < 4)
                    Line("%s.%c = %s.%c;", destination.c_str(), COMPONENTS[c], value, COMPONENTS[select]);
                else
                    Line("%s.%c = %s;", destination.c_str(), COMPONENTS[c], select == 5 ? "1.0" : "0.0");
            }
            NoteTempWrite(fetch.DstReg(), fetch.DstRelative());
        }

        void Translator::EmitVertexFetch(const Fetch& fetch)
        {
            if (IsPixel())
                return;

            uint32_t format = fetch.VertexFormat();
            bool mini = fetch.VertexIsMini();
            uint32_t fetchConstant;
            if (!mini || !m_haveFullFetch)
            {
                fetchConstant = fetch.VertexFetchConstant();
                m_lastFullFetchConstant = fetchConstant;
                m_haveFullFetch = true;

                VertexFetchBinding& binding = VertexBinding(fetchConstant);
                binding.strideDwords = fetch.VertexStride();
                bool isVertexIndex = fetch.SrcReg() == 0 && !fetch.SrcRelative() && fetch.VertexSrcComponent() == 0 && !m_r0Dirty;
                if (!isVertexIndex)
                    binding.indexedByVertexIndex = false;

                Line("xe_vf_address = xe_vb_base(%u) + int(floor(%s.%c%s)) * %d;", fetchConstant,
                     TempRegister(fetch.SrcReg(), fetch.SrcRelative()).c_str(), COMPONENTS[fetch.VertexSrcComponent()],
                     fetch.VertexIndexRounded() ? " + 0.5" : "", int(fetch.VertexStride() * 4));
            }
            else
            {
                fetchConstant = m_lastFullFetchConstant;
            }

            VertexFetchBinding& binding = VertexBinding(fetchConstant);
            int32_t offset = fetch.VertexOffset();
            uint32_t dwords = xenos::VertexFormatDwords(format);
            binding.minOffsetDwords = std::min(binding.minOffsetDwords, offset);
            binding.maxEndDwords = std::max(binding.maxEndDwords, offset + int32_t(dwords));

            if (fetch.IsPredicated())
            {
                Line("if (%sxe_p0) {", fetch.PredCondition() ? "" : "!");
            }
            else
            {
                Line("{");
            }
            m_indent++;

            Line("int a = xe_vf_address + %d;", offset * 4);
            bool isSigned = fetch.VertexSigned();
            bool normalized = !fetch.VertexInteger();
            const char* sign = isSigned ? "true" : "false";
            const char* norm = normalized ? "true" : "false";
            switch (xenos::VertexFormat(format))
            {
            case xenos::VertexFormat::k_8_8_8_8:
                Line("float4 f = xe_unpack(xe_vertex_data.Load(a).xxxx, uint4(0, 8, 16, 24), uint4(8, 8, 8, 8), %s, %s);", sign, norm);
                break;
            case xenos::VertexFormat::k_2_10_10_10:
                Line("float4 f = xe_unpack(xe_vertex_data.Load(a).xxxx, uint4(0, 10, 20, 30), uint4(10, 10, 10, 2), %s, %s);", sign, norm);
                break;
            case xenos::VertexFormat::k_10_11_11:
                Line("float4 f = float4(xe_unpack(xe_vertex_data.Load(a).xxxx, uint4(0, 11, 22, 0), uint4(11, 11, 10, 1), %s, %s).xyz, 1.0);", sign, norm);
                break;
            case xenos::VertexFormat::k_11_11_10:
                Line("float4 f = float4(xe_unpack(xe_vertex_data.Load(a).xxxx, uint4(0, 10, 21, 0), uint4(10, 11, 11, 1), %s, %s).xyz, 1.0);", sign, norm);
                break;
            case xenos::VertexFormat::k_16_16:
                Line("float4 f = float4(xe_unpack(xe_vertex_data.Load(a).xxxx, uint4(0, 16, 0, 0), uint4(16, 16, 1, 1), %s, %s).xy, 0.0, 1.0);", sign, norm);
                break;
            case xenos::VertexFormat::k_16_16_16_16:
                Line("float4 f = xe_unpack(xe_vertex_data.Load2(a).xxyy, uint4(0, 16, 0, 16), uint4(16, 16, 16, 16), %s, %s);", sign, norm);
                break;
            case xenos::VertexFormat::k_16_16_FLOAT:
                Line("uint w = xe_vertex_data.Load(a);");
                Line("float4 f = float4(f16tof32(w & 0xFFFFu), f16tof32(w >> 16), 0.0, 1.0);");
                break;
            case xenos::VertexFormat::k_16_16_16_16_FLOAT:
                Line("uint2 w = xe_vertex_data.Load2(a);");
                Line("float4 f = float4(f16tof32(w.x & 0xFFFFu), f16tof32(w.x >> 16), f16tof32(w.y & 0xFFFFu), f16tof32(w.y >> 16));");
                break;
            case xenos::VertexFormat::k_32:
                Line("float4 f = float4(xe_unpack32(xe_vertex_data.Load(a).xxxx, %s, %s).x, 0.0, 0.0, 1.0);", sign, norm);
                break;
            case xenos::VertexFormat::k_32_32:
                Line("float4 f = float4(xe_unpack32(xe_vertex_data.Load2(a).xyxy, %s, %s).xy, 0.0, 1.0);", sign, norm);
                break;
            case xenos::VertexFormat::k_32_32_32_32:
                Line("float4 f = xe_unpack32(xe_vertex_data.Load4(a), %s, %s);", sign, norm);
                break;
            case xenos::VertexFormat::k_32_FLOAT:
                Line("float4 f = float4(asfloat(xe_vertex_data.Load(a)), 0.0, 0.0, 1.0);");
                break;
            case xenos::VertexFormat::k_32_32_FLOAT:
                Line("float4 f = float4(asfloat(xe_vertex_data.Load2(a)), 0.0, 1.0);");
                break;
            case xenos::VertexFormat::k_32_32_32_FLOAT:
                Line("float4 f = float4(asfloat(xe_vertex_data.Load3(a)), 1.0);");
                break;
            case xenos::VertexFormat::k_32_32_32_32_FLOAT:
                Line("float4 f = asfloat(xe_vertex_data.Load4(a));");
                break;
            default:
                Line("float4 f = float4(0.0, 0.0, 0.0, 1.0); // unknown vertex format %u", format);
                break;
            }
            if (int32_t exp = fetch.VertexExpAdjust())
                Line("f *= %.9g;", double(ldexpf(1.0f, exp)));
            WriteFetchResult(fetch, "f");

            m_indent--;
            Line("}");
        }

        void Translator::EmitTextureFetch(const Fetch& fetch)
        {
            uint32_t opcode = fetch.Opcode();
            uint32_t fc = fetch.TextureFetchConstant();
            uint32_t dimension = fetch.TextureDimension();

            if (fetch.IsPredicated())
                Line("if (%sxe_p0) {", fetch.PredCondition() ? "" : "!");
            else
                Line("{");
            m_indent++;

            std::string src = TempRegister(fetch.SrcReg(), fetch.SrcRelative());
            uint32_t srcSwizzle = fetch.TextureSrcSwizzle();
            std::string coords = src + ".";
            for (uint32_t c = 0; c < 3; c++)
                coords += COMPONENTS[(srcSwizzle >> (2 * c)) & 3];

            switch (opcode)
            {
            case F_TEXTURE:
            case F_GET_WEIGHTS:
            case F_GET_COMPUTED_LOD:
            {
                TextureBinding& binding = TextureBindingFor(fc);
                if (binding.magFilter == 3 && binding.minFilter == 3)
                {
                    binding.dimension = dimension;
                    binding.magFilter = fetch.TextureMagFilter();
                    binding.minFilter = fetch.TextureMinFilter();
                    binding.mipFilter = fetch.TextureMipFilter();
                    binding.anisoFilter = fetch.TextureAnisoFilter();
                }
                Line("float3 uvw = %s;", coords.c_str());
                Line("float2 size; xe_t%u.GetDimensions(size.x, size.y);", fc);
                if (m_renderScaled)
                    Line("size *= xe_tex_size_scale[%u].%s; // guest texels", fc >> 1, (fc & 1) ? "zw" : "xy");
                if (fetch.TextureUnnormalized())
                    Line("uvw.xy /= size;");
                float offsetX = fetch.TextureOffset(0) * 0.5f, offsetY = fetch.TextureOffset(1) * 0.5f;
                if (offsetX != 0.0f || offsetY != 0.0f)
                    Line("uvw.xy += float2(%.9g, %.9g) / size;", double(offsetX), double(offsetY));
                if (dimension == 0)
                    Line("uvw.y = 0.5;");
                else if (dimension == 3)
                    Line("uvw.xy = uvw.xy - 1.0; // cube: face 0 only");

                if (opcode == F_TEXTURE && m_currentSlot < m_gradientFetch.size() && m_gradientFetch[m_currentSlot])
                {
                    // Mipmapped (and anisotropic) sampling with the derivatives of the input
                    // register taken in main() (FindGradientFetches), scaled by the LOD biases of
                    // the fetch constant and the instruction and by the texture LOD scale of the
                    // graphics options (xe_effect.w: below 1 when an upscaler draws the scene smaller).
                    uint32_t reg = fetch.SrcReg();
                    char c0 = COMPONENTS[srcSwizzle & 3], c1 = COMPONENTS[(srcSwizzle >> 2) & 3];
                    Line("float2 gx = float2(xe_gx_r%u.%c, xe_gx_r%u.%c), gy = float2(xe_gy_r%u.%c, xe_gy_r%u.%c);", reg, c0, reg, c1, reg, c0, reg, c1);
                    if (fetch.TextureUnnormalized())
                        Line("gx /= size; gy /= size;");
                    Line("float gs = exp2(xe_lod_bias(%u) + %.9g) * xe_effect.w;", fc, double(fetch.TextureLodBias()) / 16.0);
                    Line("float4 t = xe_swizzle(xe_t%u.SampleGrad(xe_s%u, uvw.xy, gx * gs, gy * gs), %u);", fc, fc, fc);
                }
                else if (opcode == F_TEXTURE)
                {
                    // Explicit LOD 0: no derivatives here (control flow, computed coordinates).
                    Line("float4 t = xe_swizzle(xe_t%u.SampleLevel(xe_s%u, uvw.xy, 0.0), %u);", fc, fc, fc);
                }
                else if (opcode == F_GET_WEIGHTS)
                {
                    Line("float4 t = float4(frac(uvw.xy * size - 0.5), 0.0, 0.0);");
                }
                else
                {
                    Line("float4 t = 0.0;");
                }
                WriteFetchResult(fetch, "t");
                break;
            }
            case F_GET_GRADIENTS:
                if (IsPixel() && !m_out.usesControlFlowMachine)
                {
                    Line("float2 c = %s.xy;", src.c_str());
                    Line("float4 t = float4(ddx(c.x), ddy(c.x), ddx(c.y), ddy(c.y));");
                }
                else
                {
                    Line("float4 t = 0.0;");
                }
                WriteFetchResult(fetch, "t");
                break;
            case F_GET_BORDER_COLOR_FRAC:
                Line("float4 t = 0.0;");
                WriteFetchResult(fetch, "t");
                break;
            default:
                // setTexLOD / setGradientH/V: state only used by later fetches; ignored.
                break;
            }

            m_indent--;
            Line("}");
        }

        void Translator::EmitFetch(const Fetch& fetch)
        {
            if (fetch.Opcode() == F_VERTEX)
                EmitVertexFetch(fetch);
            else
                EmitTextureFetch(fetch);
        }

        void Translator::EmitExecBody(const Cf& cf)
        {
            for (uint32_t i = 0; i < cf.ExecCount(); i++)
            {
                uint32_t slot = cf.ExecAddress() + i;
                if (slot * 3 + 3 > m_dwordCount)
                {
                    Line("// instruction slot %u is outside the microcode", slot);
                    break;
                }
                const uint32_t* words = m_ucode + slot * 3;
                m_currentSlot = slot;
                m_out.instructionCount++;
                if ((cf.ExecSequence() >> (i * 2)) & 1)
                {
                    Line("// %u: fetch %08X %08X %08X", slot, words[0], words[1], words[2]);
                    EmitFetch(Fetch{ words[0], words[1], words[2] });
                }
                else
                {
                    Line("// %u: alu %08X %08X %08X", slot, words[0], words[1], words[2]);
                    EmitAlu(Alu{ words[0], words[1], words[2] });
                }
            }
        }

        // Straight-line control flow (no loops, calls or jumps): execs become optional blocks.
        void Translator::EmitStructured()
        {
            for (size_t index = 0; index < m_cf.size(); index++)
            {
                const Cf& cf = m_cf[index];
                if (!cf.IsExec())
                    continue;
                std::string condition = ExecCondition(cf);
                if (!condition.empty())
                {
                    Line("if (%s) {", condition.c_str());
                    m_indent++;
                }
                EmitExecBody(cf);
                if (cf.EndsShader())
                    Line("return;");
                if (!condition.empty())
                {
                    m_indent--;
                    Line("}");
                }
                if (cf.EndsShader() && condition.empty())
                    break;
            }
        }

        // General control flow: a program counter over the control flow instructions.
        void Translator::EmitStateMachine()
        {
            Line("int xe_pc = 0;");
            Line("int xe_loop_depth = -1;");
            Line("int xe_call_depth = 0;");
            Line("int xe_loop_iterator[4] = { 0, 0, 0, 0 };");
            Line("int xe_loop_start[4] = { 0, 0, 0, 0 };");
            Line("int xe_loop_step[4] = { 0, 0, 0, 0 };");
            Line("int xe_call_return[4] = { 0, 0, 0, 0 };");
            Line("[loop] for (int xe_guard = 0; xe_guard < 65536; xe_guard++) {");
            m_indent++;
            Line("[forcecase] switch (xe_pc) {");
            for (size_t index = 0; index < m_cf.size(); index++)
            {
                const Cf& cf = m_cf[index];
                uint32_t next = uint32_t(index + 1);
                Line("case %u: {", uint32_t(index));
                m_indent++;
                switch (cf.Opcode())
                {
                case CF_EXEC: case CF_EXEC_END: case CF_COND_EXEC: case CF_COND_EXEC_END:
                case CF_COND_EXEC_PRED: case CF_COND_EXEC_PRED_END:
                case CF_COND_EXEC_PRED_CLEAN: case CF_COND_EXEC_PRED_CLEAN_END:
                {
                    std::string condition = ExecCondition(cf);
                    if (!condition.empty())
                    {
                        Line("if (%s) {", condition.c_str());
                        m_indent++;
                    }
                    EmitExecBody(cf);
                    if (cf.EndsShader())
                        Line("return;");
                    if (!condition.empty())
                    {
                        m_indent--;
                        Line("}");
                    }
                    Line("xe_pc = %u;", next);
                    break;
                }
                case CF_LOOP_START:
                {
                    uint32_t id = cf.LoopId();
                    Line("uint lc = xe_loop_consts[%u].%c;", id >> 2, COMPONENTS[id & 3]);
                    Line("xe_loop_depth = min(xe_loop_depth + 1, 3);");
                    if (!cf.LoopIsRepeat())
                        Line("xe_loop_iterator[xe_loop_depth] = 0;");
                    Line("xe_loop_start[xe_loop_depth] = int(lc >> 8) & 0xFF;");
                    Line("xe_loop_step[xe_loop_depth] = int(lc << 8) >> 24;");
                    Line("xe_aL = xe_loop_iterator[xe_loop_depth] * xe_loop_step[xe_loop_depth] + xe_loop_start[xe_loop_depth];");
                    Line("if (uint(xe_loop_iterator[xe_loop_depth]) >= (lc & 0xFFu)) {");
                    Line("    xe_loop_depth--;");
                    Line("    if (xe_loop_depth >= 0) xe_aL = xe_loop_iterator[xe_loop_depth] * xe_loop_step[xe_loop_depth] + xe_loop_start[xe_loop_depth];");
                    Line("    xe_pc = %u;", cf.TargetAddress());
                    Line("} else {");
                    Line("    xe_pc = %u;", next);
                    Line("}");
                    break;
                }
                case CF_LOOP_END:
                {
                    uint32_t id = cf.LoopId();
                    Line("uint lc = xe_loop_consts[%u].%c;", id >> 2, COMPONENTS[id & 3]);
                    Line("if (xe_loop_depth < 0) { xe_pc = %u; break; }", next);
                    Line("xe_loop_iterator[xe_loop_depth]++;");
                    Line("xe_aL = xe_loop_iterator[xe_loop_depth] * xe_loop_step[xe_loop_depth] + xe_loop_start[xe_loop_depth];");
                    std::string breakCondition = cf.LoopEndPredicatedBreak() ? (cf.Condition() ? "xe_p0" : "!xe_p0") : "false";
                    Line("if (uint(xe_loop_iterator[xe_loop_depth]) < (lc & 0xFFu) && !(%s)) {", breakCondition.c_str());
                    Line("    xe_pc = %u;", cf.TargetAddress());
                    Line("} else {");
                    Line("    xe_loop_depth--;");
                    Line("    if (xe_loop_depth >= 0) xe_aL = xe_loop_iterator[xe_loop_depth] * xe_loop_step[xe_loop_depth] + xe_loop_start[xe_loop_depth];");
                    Line("    xe_pc = %u;", next);
                    Line("}");
                    break;
                }
                case CF_COND_CALL:
                case CF_COND_JMP:
                {
                    std::string condition = "true";
                    if (!cf.IsUnconditional())
                    {
                        if (cf.IsPredicated())
                            condition = cf.Condition() ? "xe_p0" : "!xe_p0";
                        else
                            condition = BoolConstant(cf.BoolAddress()) + (cf.Condition() ? " != 0u" : " == 0u");
                    }
                    Line("if (%s) {", condition.c_str());
                    if (cf.Opcode() == CF_COND_CALL)
                        Line("    if (xe_call_depth < 4) { xe_call_return[xe_call_depth] = %u; xe_call_depth++; }", next);
                    Line("    xe_pc = %u;", cf.TargetAddress());
                    Line("} else {");
                    Line("    xe_pc = %u;", next);
                    Line("}");
                    break;
                }
                case CF_RETURN:
                    Line("if (xe_call_depth > 0) { xe_call_depth--; xe_pc = xe_call_return[xe_call_depth]; } else { xe_pc = %u; }", next);
                    break;
                default:
                    Line("xe_pc = %u;", next);
                    break;
                }
                Line("break;");
                m_indent--;
                Line("}");
            }
            Line("default:");
            Line("    return;");
            Line("}");
            m_indent--;
            Line("}");
        }

        // Shared HLSL: constant buffers and helper functions.
        const char* COMMON_HLSL = R"(
cbuffer xe_system : register(b0)
{
    float4 xe_ndc_scale;        // xyz
    float4 xe_ndc_offset;       // xyz
    uint4 xe_flags;             // x: flags, y: alpha test function, z: param gen register, w: unused
    float4 xe_alpha_ref;        // x: alpha reference
    float4 xe_color_exp_bias;   // multiplier per color target
    uint4 xe_bool_consts[2];
    uint4 xe_loop_consts[8];
    int4 xe_vb_bases[24];       // byte offset of vertex fetch constant N's base in xe_vertex_data
    uint4 xe_tex_swizzles[8];   // texture fetch constant swizzles (12 bits)
    float4 xe_tex_lod_bias[8];  // texture fetch constant LOD biases
};

// Graphics options (gpu/graphics_options.h): root constants, per draw.
cbuffer xe_effects : register(b3)
{
    float4 xe_effect;           // x: motion blur, y: depth of field, z: shadow softness (scales), w: texture LOD scale
    float4 xe_jitter;           // xy: sub-pixel jitter in NDC for perspective draws (temporal upscaling)
    float4 xe_hud;              // wide screens (gpu/widescreen.h): xy scale of the 2D around the centre, z: mode bits, w: anchor shift
    float4 xe_hud_target;       // xy: the target's guest size of the NDC, zw: the screen's (1280x720)
};

// Wide screens: a 2D vertex (NDC) back into the middle 16:9 area of the screen (gpu/widescreen.h,
// HudTransform in xenos_renderer.cpp). Mode 1: scaled around the centre except the coordinates at
// the screen's edges (full-screen quads still cover it); 2: also a vertex at a corner zooms out of
// the screen the other way (backgrounds keep their aspect and fill it); 4: just scaled (videos);
// 8: moved by xe_hud.w (signed), an element of a HUD corner kept at its screen edge; 32: done
// per triangle by HUD_GS (xenos_renderer.cpp) instead, for triangles.
float2 xe_hud_transform(float2 ndc)
{
    uint mode = uint(xe_hud.z + 0.5);
    float2 px = float2((ndc.x + 1.0) * 0.5 * xe_hud_target.x, (1.0 - ndc.y) * 0.5 * xe_hud_target.y);
    float2 centre = xe_hud_target.zw * 0.5;
    float2 d = px - centre;
    float2 result = centre + d * xe_hud.xy;
    if (!(mode & 4u))
    {
        bool2 edge = bool2(px.x <= 2.0 || px.x >= xe_hud_target.z - 2.0, px.y <= 2.0 || px.y >= xe_hud_target.w - 2.0);
        if (mode & 8u)
            result.x += xe_hud.w;  // the draw's side (HudDrawSide in xenos_renderer.cpp)
        if (edge.x) result.x = px.x;
        if (edge.y) result.y = px.y;
        if ((mode & 2u) && edge.x && edge.y) result = centre + d * float2(1.0 / xe_hud.y, 1.0 / xe_hud.x);
    }
    return float2(result.x / xe_hud_target.x * 2.0 - 1.0, 1.0 - result.y / xe_hud_target.y * 2.0);
}

#define XE_FLAG_XY_DIVIDED_BY_W 1u
#define XE_FLAG_Z_DIVIDED_BY_W 2u
#define XE_FLAG_W_NOT_RECIPROCAL 4u
#define XE_FLAG_PARAM_GEN 8u
#define XE_FLAG_ALPHA_TEST 16u

ByteAddressBuffer xe_vertex_data : register(t0, space1);

static float4 xe_o[16];
static float4 xe_o_pos;
static float4 xe_o_misc;
static float4 xe_oC[4];
static float xe_oDepth;
static float xe_ps;
static bool xe_p0;
static int xe_a0;
static int xe_aL;
static int xe_vf_address;

int xe_vb_base(uint index) { return xe_vb_bases[index >> 2][index & 3]; }
float xe_lod_bias(uint index) { return xe_tex_lod_bias[index >> 2][index & 3]; }

float4 xe_mul(float4 a, float4 b) { return (a == 0.0 || b == 0.0) ? 0.0 : a * b; }
float xe_muls(float a, float b) { return (a == 0.0 || b == 0.0) ? 0.0 : a * b; }
float4 xe_max(float4 a, float4 b) { return a >= b ? a : b; }
float4 xe_min(float4 a, float4 b) { return a < b ? a : b; }
float xe_dp4(float4 a, float4 b) { float4 m = xe_mul(a, b); return m.x + m.y + m.z + m.w; }
float xe_dp3(float4 a, float4 b) { float4 m = xe_mul(a, b); return m.x + m.y + m.z; }
float xe_dp2(float4 a, float4 b) { float4 m = xe_mul(a, b); return m.x + m.y; }
float xe_max4(float4 a)
{
    if (a.x >= a.y && a.x >= a.z && a.x >= a.w) return a.x;
    if (a.y >= a.z && a.y >= a.w) return a.y;
    return a.z >= a.w ? a.z : a.w;
}
float4 xe_cube(float4 s)
{
    float x = s.z, y = s.w, z = s.x;
    float4 r;
    if (abs(z) >= abs(x) && abs(z) >= abs(y))
        r = float4(-y, z < 0.0 ? -x : x, z, z < 0.0 ? 5.0 : 4.0);
    else if (abs(y) >= abs(x))
        r = float4(y < 0.0 ? -z : z, x, y, y < 0.0 ? 3.0 : 2.0);
    else
        r = float4(-y, x < 0.0 ? z : -z, x, x < 0.0 ? 1.0 : 0.0);
    r.z *= 2.0;
    return r;
}
float xe_select(float4 v, uint s)
{
    if (s >= 4u) return s == 5u ? 1.0 : 0.0;
    return dot(v, float4(s == 0u, s == 1u, s == 2u, s == 3u));
}
float4 xe_swizzle(float4 v, uint index)
{
    uint s = xe_tex_swizzles[index >> 2][index & 3];
    return float4(xe_select(v, s & 7u), xe_select(v, (s >> 3) & 7u), xe_select(v, (s >> 6) & 7u), xe_select(v, (s >> 9) & 7u));
}
float4 xe_unpack(uint4 w, uint4 offset, uint4 width, bool isSigned, bool normalized)
{
    float4 f;
    if (isSigned)
    {
        int4 v = int4(w << (32u - offset - width)) >> int4(32u - width);
        f = float4(v);
        if (normalized) f = max(f / float4((1u << (width - 1u)) - 1u), -1.0);
    }
    else
    {
        uint4 v = (w >> offset) & ((1u << width) - 1u);
        f = float4(v);
        if (normalized) f /= float4((1u << width) - 1u);
    }
    return f;
}
float4 xe_unpack32(uint4 w, bool isSigned, bool normalized)
{
    float4 f = isSigned ? float4(asint(w)) : float4(w);
    if (normalized) f /= isSigned ? 2147483647.0 : 4294967295.0;
    return f;
}
)";
    }

    TranslatedShader Translator::Run()
    {
        m_out.stage = m_stage;
        m_out.hash = HashUcode(m_stage, m_ucode, m_dwordCount);
        ParseControlFlow();
        ScanRegisters();
        FindGradientFetches();

        uint32_t registerCount = m_relativeTemps ? 64 : m_maxRegister + 1;
        if (IsPixel())
            registerCount = std::max(registerCount, 16u);

        if (m_out.usesControlFlowMachine)
            EmitStateMachine();
        else
            EmitStructured();

        std::string& hlsl = m_out.hlsl;
        Append(hlsl, "// Xenos %s shader %016llX, %u control flow instructions\n", IsPixel() ? "pixel" : "vertex",
               (unsigned long long)m_out.hash, uint32_t(m_cf.size()));
        if (m_renderScaled)
        {
            std::string common = COMMON_HLSL;
            const std::string last = "    float4 xe_tex_lod_bias[8];  // texture fetch constant LOD biases\n";
            common.insert(common.find(last) + last.size(),
                          "    float4 xe_render_scale;     // xy: guest pixels per host pixel (internal resolution)\n"
                          "    float4 xe_tex_size_scale[16]; // xy / zw: guest texels per host texel, per fetch constant\n");
            hlsl += common;
        }
        else
        {
            hlsl += COMMON_HLSL;
        }
        Append(hlsl, "cbuffer xe_float_consts : register(%s) { float4 xe_c[256]; };\n", IsPixel() ? "b2" : "b1");
        for (const TextureBinding& binding : m_out.textures)
            Append(hlsl, "Texture2D<float4> xe_t%u : register(t%u);\nSamplerState xe_s%u : register(s%u);\n",
                   binding.fetchConstant, binding.fetchConstant, binding.fetchConstant, binding.fetchConstant);
        Append(hlsl, "static float4 r[%u];\n", registerCount);
        for (uint32_t reg = 0; reg < 16; reg++)
        {
            if (m_gradientRegisters & (1u << reg))
                Append(hlsl, "static float4 xe_gx_r%u, xe_gy_r%u;  // derivatives of input register %u\n", reg, reg, reg);
        }
        hlsl += "\n";

        hlsl += "void xe_main()\n{\n";
        hlsl += m_body;
        hlsl += "}\n\n";

        hlsl += "struct XeVertexOutput\n{\n    float4 interpolators[16] : TEXCOORD0;\n    float4 position : SV_Position;\n};\n\n";

        if (!IsPixel())
        {
            hlsl += "XeVertexOutput main(uint vertex_index : SV_VertexID)\n{\n";
            Append(hlsl, "    [unroll] for (uint i = 0; i < %uu; i++) r[i] = 0.0;\n", registerCount);
            hlsl += R"(    [unroll] for (uint j = 0; j < 16u; j++) xe_o[j] = 0.0;
    xe_o_pos = float4(0.0, 0.0, 0.0, 1.0);
    xe_o_misc = 0.0;
    xe_ps = 0.0; xe_p0 = false; xe_a0 = 0; xe_aL = 0; xe_vf_address = 0;
    r[0].x = float(vertex_index);
    xe_main();
    XeVertexOutput output;
    output.interpolators = xe_o;
    float4 p = xe_o_pos;
    uint flags = xe_flags.x;
    // PA_CL_VTE_CNTL.vtx_w0_fmt = 1: the shader wrote W itself; 0: it wrote 1/W.
    if (!(flags & XE_FLAG_W_NOT_RECIPROCAL)) p.w = 1.0 / p.w;
    if (flags & XE_FLAG_XY_DIVIDED_BY_W) p.xy *= p.w;
    if (flags & XE_FLAG_Z_DIVIDED_BY_W) p.z *= p.w;
    p.xyz = p.xyz * xe_ndc_scale.xyz + xe_ndc_offset.xyz * p.w;
    // Wide screens: 2D (W = 1) into the middle 16:9 area.
    if (xe_hud.z > 0.5 && xe_hud.z < 31.5 && abs(p.w - 1.0) <= 1e-5) p.xy = xe_hud_transform(p.xy);
    // Temporal upscaling: jitter perspective geometry only (screen-space quads have W = 1).
    if (abs(p.w - 1.0) > 1e-5) p.xy += xe_jitter.xy * p.w;
    output.position = p;
    return output;
}
)";
        }
        else
        {
            hlsl += "struct XePixelOutput\n{\n";
            for (uint32_t i = 0; i < 4; i++)
            {
                if (m_out.colorTargetsWritten & (1u << i))
                    Append(hlsl, "    float4 color%u : SV_Target%u;\n", i, i);
            }
            if (m_out.writesDepth)
                hlsl += "    float depth : SV_Depth;\n";
            if (!m_out.colorTargetsWritten && !m_out.writesDepth)
                hlsl += "    float4 color0 : SV_Target0;\n";
            hlsl += "};\n\n";
            hlsl += "XePixelOutput main(XeVertexOutput input, bool front_face : SV_IsFrontFace)\n{\n";
            Append(hlsl, "    [unroll] for (uint i = 0; i < %uu; i++) r[i] = i < 16u ? input.interpolators[min(i, 15u)] : 0.0;\n", registerCount);
            hlsl += R"(    [unroll] for (uint j = 0; j < 4u; j++) xe_oC[j] = 0.0;
    xe_oDepth = input.position.z;
    xe_ps = 0.0; xe_p0 = false; xe_a0 = 0; xe_aL = 0; xe_vf_address = 0;
    uint flags = xe_flags.x;
    if (flags & XE_FLAG_PARAM_GEN)
    {
        float4 param = float4(input.position.xy - 0.5, 0.0, 0.0);
        if (!front_face) param.x = -param.x;
        [unroll] for (uint k = 0; k < 16u; k++)
        {
            if (k == xe_flags.z) r[k] = param;
        }
    }
)";
            for (uint32_t reg = 0; reg < 16; reg++)
            {
                if (m_gradientRegisters & (1u << reg))
                    Append(hlsl, "    xe_gx_r%u = ddx(r[%u]); xe_gy_r%u = ddy(r[%u]);\n", reg, reg, reg, reg);
            }
            hlsl += R"(    xe_main();
    if (flags & XE_FLAG_ALPHA_TEST)
    {
        float alpha = xe_oC[0].a;
        float ref = xe_alpha_ref.x;
        bool alphaPass;
        switch (xe_flags.y)
        {
        case 0u: alphaPass = false; break;
        case 1u: alphaPass = alpha < ref; break;
        case 2u: alphaPass = alpha == ref; break;
        case 3u: alphaPass = alpha <= ref; break;
        case 4u: alphaPass = alpha > ref; break;
        case 5u: alphaPass = alpha != ref; break;
        case 6u: alphaPass = alpha >= ref; break;
        default: alphaPass = true; break;
        }
        if (!alphaPass) discard;
    }
    XePixelOutput output;
)";
            for (uint32_t i = 0; i < 4; i++)
            {
                if (m_out.colorTargetsWritten & (1u << i))
                    Append(hlsl, "    output.color%u = xe_oC[%u] * xe_color_exp_bias[%u];\n", i, i, i);
            }
            if (m_out.writesDepth)
                hlsl += "    output.depth = saturate(xe_oDepth);\n";
            if (!m_out.colorTargetsWritten && !m_out.writesDepth)
                hlsl += "    output.color0 = 0.0;\n";
            hlsl += "    return output;\n}\n";
        }

        if (m_renderScaled)
        {
            // Param gen's pixel position in guest pixels.
            const std::string position = "float4(input.position.xy - 0.5,";
            if (size_t at = hlsl.find(position); at != std::string::npos)
                hlsl.replace(at, position.size(), "float4(input.position.xy * xe_render_scale.xy - 0.5,");
        }
        return std::move(m_out);
    }

    uint64_t HashUcode(ShaderStage stage, const uint32_t* ucode, uint32_t dwordCount)
    {
        // FNV-1a over the dwords, seeded with the stage.
        uint64_t hash = 0xCBF29CE484222325ull ^ (uint64_t(stage) * 0x9E3779B97F4A7C15ull);
        for (uint32_t i = 0; i < dwordCount; i++)
        {
            hash ^= ucode[i];
            hash *= 0x100000001B3ull;
        }
        return hash;
    }

    void SetRenderScaled(bool scaled)
    {
        g_renderScaled = scaled;
    }

    TranslatedShader TranslateShader(ShaderStage stage, const uint32_t* ucode, uint32_t dwordCount)
    {
        return Translator(stage, ucode, dwordCount, g_renderScaled).Run();
    }

    TranslatedShader TranslateShader(ShaderStage stage, const uint32_t* ucode, uint32_t dwordCount, bool renderScaled)
    {
        return Translator(stage, ucode, dwordCount, renderScaled).Run();
    }
}
