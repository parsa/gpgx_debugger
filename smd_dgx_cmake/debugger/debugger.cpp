#include "debugger.h"
#include "grpcpp/server_builder.h"

#include "debug_proto_68k.grpc.pb.h"

#include <thread>
#include <format>

namespace 
{
    using namespace grpc;
    using namespace google::protobuf;
    using namespace idadebug;

    struct debugger_impl
    {
		virtual ~debugger_impl() = default;

        std::vector<uint32> callstack;
        std::map<uint32_t, uint32_t> changed;
        std::vector<breakpoint_t> breakpoints;

        bool debug_stop = false;
        bool step_into = false;
        uint32_t step_over = -1;

        bool handled_ida_event = false;

        void breakpoint(int in_pc)
        {
            if (!handled_ida_event)
            {
                send_pause_event(in_pc, changed);
                changed.clear();
            }
        }

        bool break_pc(int in_pc)
        {
	        
        }
    };

	class gx_server_handler final : public idadebug::DbgServer::Service
	{
		grpc::Status add_breakpoint(grpc::ServerContext* context, const idadebug::DbgBreakpoint* request, google::protobuf::Empty* response) override {
#ifdef DEBUG_68K
            breakpoint_t b((e_bp_type)request->type(), request->bstart() & 0xFFFFFF, request->bend() & 0xFFFFFF, true, request->is_vdp(), request->elang(), request->condition());
            M68kDW.Breakpoints.push_back(b);
#else
            Breakpoint b((bp_type)request->type(), request->bstart() & 0xFFFFFF, request->bend() & 0xFFFFFF, true, request->elang(), request->condition());
            Z80DW.Breakpoints.push_back(b);
#endif

            return grpc::Status::OK;
        }

        Status clear_breakpoints(ServerContext* context, const Empty* request, Empty* response) override {
#ifdef DEBUG_68K
            M68kDW.Breakpoints.clear();
#else
            Z80DW.Breakpoints.clear();
#endif

            return Status::OK;
        }

        Status del_breakpoint(ServerContext* context, const DbgBreakpoint* request, Empty* response) override {
#ifdef DEBUG_68K
            for (auto i = M68kDW.Breakpoints.begin(); i != M68kDW.Breakpoints.end(); ++i) {
#else
            for (auto i = Z80DW.Breakpoints.begin(); i != Z80DW.Breakpoints.end(); ++i) {
#endif
                if (request->type() == (BpType)i->type && request->bstart() == i->start) {
#ifdef DEBUG_68K
                    if (request->is_vdp() == i->is_vdp) {
                        M68kDW.Breakpoints.erase(i);
                        break;
                    }
#else
                    Z80DW.Breakpoints.erase(i);
                    break;
#endif
                }
            }

            return Status::OK;
            }

#ifdef DEBUG_Z80
        Status get_sound_banks(ServerContext * context, const Empty * request, SoundBankMap * response) override {
            response->clear_range();

            for (auto i = z80_banks.cbegin(); i != z80_banks.cend(); ++i) {
                SoundBankRange bnk;
                bnk.set_bank_min(i->second.bank_min);
                bnk.set_bank_max(i->second.bank_max);

                (*response->mutable_range())[i->first] = bnk;
            }

            return Status::OK;
        }
#endif

        Status exit_emulation(ServerContext * context, const Empty * request, Empty * response) override {
            if (!client) {
                return Status::CANCELLED;
            }

#ifdef DEBUG_68K
            client->stop_event(M68kDW.changed);
#else
            client->stop_event(Z80DW.changed);
#endif

            SendMessageA(HWnd, WM_CLOSE, 0, 0);

            return Status::OK;
        }

        Status get_breakpoints(ServerContext * context, const Empty * request, DbgBreakpoints * response) override {
#ifdef DEBUG_68K
            for (auto i = M68kDW.Breakpoints.cbegin(); i != M68kDW.Breakpoints.cend(); ++i) {
#else
            for (auto i = Z80DW.Breakpoints.cbegin(); i != Z80DW.Breakpoints.cend(); ++i) {
#endif
                DbgBreakpoint* bpt = response->add_list();
                bpt->set_enabled(i->enabled);
#ifdef DEBUG_68K
                bpt->set_is_vdp(i->is_vdp);
#endif
                bpt->set_elang(i->elang);
                bpt->set_condition(i->condition.c_str());
                bpt->set_bstart(i->start);
                bpt->set_bend(i->end);
                bpt->set_type((BpType)i->type);
            }

            return Status::OK;
            }

        Status get_callstack(ServerContext * context, const Empty * request, Callstack * response) override {
            response->clear_callstack();
#ifdef DEBUG_68K
            for (auto i = M68kDW.callstack.cbegin(); i != M68kDW.callstack.cend(); ++i) {
#else
            for (auto i = Z80DW.callstack.cbegin(); i != Z80DW.callstack.cend(); ++i) {
#endif
                response->add_callstack(*i);
            }

            return Status::OK;
            }

#ifdef DEBUG_68K
        Status get_dma_info(ServerContext * context, const Empty * request, DmaInfo * response) override {
            response->set_len((BYTE)(VDP_Reg.regs[VdpRegsEnum::V13]) | ((BYTE)(VDP_Reg.regs[VdpRegsEnum::V14]) << 8));

            uint32_t src = (BYTE)(VDP_Reg.regs[VdpRegsEnum::V15]) | ((BYTE)(VDP_Reg.regs[VdpRegsEnum::V16]) << 8);

            UINT16 dma_high = VDP_Reg.regs[VdpRegsEnum::V17];
            if (!(dma_high & 0x80)) {
                src |= ((BYTE)(VDP_Reg.regs[VdpRegsEnum::V17] & mask(0, 7)) << 16);
            }
            else {
                src |= ((BYTE)(VDP_Reg.regs[VdpRegsEnum::V17] & mask(0, 6)) << 16);
            }
            src <<= 1;
            src &= 0xFFFFFF;

            response->set_src(src);

            uint32_t dst = BREAKPOINTS_BASE;
            switch (Ctrl.Access) {
            case 0x09: // VRAM
            case 0x0A: // CRAM
            case 0x0B: // VSRAM
                dst = (BREAKPOINTS_BASE + 0x10000 * (Ctrl.Access - 0x09)) + (Ctrl.Address & 0xFFFF);
                break;
            }

            response->set_dst(dst);

            return Status::OK;
        }
#endif

        Status get_gp_reg(ServerContext * context, const GpReg * request, AnyRegValue * response) override {
#ifdef DEBUG_68K
            if (request->reg() >= GpRegsEnum::D0 && request->reg() <= GpRegsEnum::D7) { // Dx
                response->set_value(main68k_context.dreg[(int)request->reg()]);
                return Status::OK;
            }
            else if (request->reg() >= GpRegsEnum::A0 && request->reg() <= GpRegsEnum::A7) { // Ax
                response->set_value(main68k_context.areg[request->reg() - GpRegsEnum::A0]);
                return Status::OK;
            }
            else {
                switch (request->reg()) {
                case GpRegsEnum::PC: {
                    response->set_value(main68k_context.pc & 0xFFFFFF);
                    return Status::OK;
                }
                case GpRegsEnum::SR: {
                    response->set_value(main68k_context.sr);
                    return Status::OK;
                }
                case GpRegsEnum::SP: {
                    response->set_value(main68k_context.areg[GpRegsEnum::A7 - GpRegsEnum::A0]);
                    return Status::OK;
                }
                }
            }
#else
            switch (request->reg()) {
            case GpRegsEnum::AF: {
                response->set_value((M_Z80.AF.b.A << 8) | (M_Z80.AF.b.F));
                return Status::OK;
            }
            case GpRegsEnum::BC: {
                response->set_value(M_Z80.BC.w.BC);
                return Status::OK;
            }
            case GpRegsEnum::DE: {
                response->set_value(M_Z80.DE.w.DE);
                return Status::OK;
            }
            case GpRegsEnum::HL: {
                response->set_value(M_Z80.HL.w.HL);
                return Status::OK;
            }
            case GpRegsEnum::IX: {
                response->set_value(M_Z80.IX.w.IX);
                return Status::OK;
            }
            case GpRegsEnum::IY: {
                response->set_value(M_Z80.IY.w.IY);
                return Status::OK;
            }

            case GpRegsEnum::A: {
                response->set_value(M_Z80.AF.b.A);
                return Status::OK;
            }
            case GpRegsEnum::B: {
                response->set_value(M_Z80.BC.b.B);
                return Status::OK;
            }
            case GpRegsEnum::C: {
                response->set_value(M_Z80.BC.b.C);
                return Status::OK;
            }
            case GpRegsEnum::D: {
                response->set_value(M_Z80.DE.b.D);
                return Status::OK;
            }
            case GpRegsEnum::E: {
                response->set_value(M_Z80.DE.b.E);
                return Status::OK;
            }
            case GpRegsEnum::H: {
                response->set_value(M_Z80.HL.b.H);
                return Status::OK;
            }
            case GpRegsEnum::L: {
                response->set_value(M_Z80.HL.b.L);
                return Status::OK;
            }

            case GpRegsEnum::IXH: {
                response->set_value(M_Z80.IX.b.IXH);
                return Status::OK;
            }
            case GpRegsEnum::IXL: {
                response->set_value(M_Z80.IX.b.IXL);
                return Status::OK;
            }

            case GpRegsEnum::IYH: {
                response->set_value(M_Z80.IY.b.IYH);
                return Status::OK;
            }
            case GpRegsEnum::IYL: {
                response->set_value(M_Z80.IY.b.IYL);
                return Status::OK;
            }

            case GpRegsEnum::AF2: {
                response->set_value((M_Z80.AF2.b.A2 << 8) | (M_Z80.AF2.b.F2));
                return Status::OK;
            }
            case GpRegsEnum::BC2: {
                response->set_value(M_Z80.BC2.w.BC2);
                return Status::OK;
            }
            case GpRegsEnum::DE2: {
                response->set_value(M_Z80.DE2.w.DE2);
                return Status::OK;
            }
            case GpRegsEnum::HL2: {
                response->set_value(M_Z80.HL2.w.HL2);
                return Status::OK;
            }

            case GpRegsEnum::I: {
                response->set_value(M_Z80.I);
                return Status::OK;
            }
            case GpRegsEnum::R: {
                response->set_value(M_Z80.R.w.R);
                return Status::OK;
            }

            case GpRegsEnum::SP: {
                response->set_value(M_Z80.SP.w.SP);
                return Status::OK;
            }
            case GpRegsEnum::IP: {
                response->set_value(Z80DW.last_pc);
                return Status::OK;
            }

            case GpRegsEnum::BANK: {
                response->set_value(Bank_Z80);
                return Status::OK;
            }
            }
#endif

            return Status::OK;
        }

        Status get_gp_regs(ServerContext * context, const Empty * request, GpRegs * response) override {
#ifdef DEBUG_68K
            response->set_d0(main68k_context.dreg[0]);
            response->set_d1(main68k_context.dreg[1]);
            response->set_d2(main68k_context.dreg[2]);
            response->set_d3(main68k_context.dreg[3]);
            response->set_d4(main68k_context.dreg[4]);
            response->set_d5(main68k_context.dreg[5]);
            response->set_d6(main68k_context.dreg[6]);
            response->set_d7(main68k_context.dreg[7]);

            response->set_a0(main68k_context.areg[0]);
            response->set_a1(main68k_context.areg[1]);
            response->set_a2(main68k_context.areg[2]);
            response->set_a3(main68k_context.areg[3]);
            response->set_a4(main68k_context.areg[4]);
            response->set_a5(main68k_context.areg[5]);
            response->set_a6(main68k_context.areg[6]);
            response->set_a7(main68k_context.areg[7]);
            response->set_sp(main68k_context.areg[7]);

            response->set_pc(M68kDW.last_pc & 0xFFFFFF);
            response->set_sr(main68k_context.sr);
#else
            response->set_af((M_Z80.AF.b.A << 8) | M_Z80.AF.b.F);
            response->set_bc(M_Z80.BC.w.BC);
            response->set_de(M_Z80.DE.w.DE);
            response->set_hl(M_Z80.HL.w.HL);

            response->set_ix(M_Z80.IX.w.IX);
            response->set_iy(M_Z80.IY.w.IY);

            response->set_a(M_Z80.AF.b.A);
            response->set_b(M_Z80.BC.b.B);
            response->set_c(M_Z80.BC.b.C);
            response->set_d(M_Z80.DE.b.D);
            response->set_e(M_Z80.DE.b.E);
            response->set_h(M_Z80.HL.b.H);
            response->set_l(M_Z80.HL.b.L);

            response->set_ixh(M_Z80.IX.b.IXH);
            response->set_ixl(M_Z80.IX.b.IXL);
            response->set_iyh(M_Z80.IY.b.IYH);
            response->set_iyl(M_Z80.IY.b.IYL);


            response->set_af2((M_Z80.AF2.b.A2 << 8) | M_Z80.AF2.b.F2);
            response->set_bc2(M_Z80.BC2.w.BC2);
            response->set_de2(M_Z80.DE2.w.DE2);
            response->set_hl2(M_Z80.HL2.w.HL2);

            response->set_i(M_Z80.I);
            response->set_r(M_Z80.R.w.R);

            response->set_sp(M_Z80.SP.w.SP);
            response->set_ip(Z80DW.last_pc);

            response->set_bank(Bank_Z80);
#endif

            return Status::OK;
        }

#ifdef DEBUG_68K
        Status get_vdp_reg(ServerContext * context, const VdpReg * request, AnyRegValue * response) override {
            if (request->reg() >= VdpRegsEnum::V00 && request->reg() <= VdpRegsEnum::V17) {
                response->set_value(VDP_Reg.regs[(int)request->reg()]);
                return Status::OK;
            }

            return Status::OK;
        }

        Status get_vdp_regs(ServerContext * context, const Empty * request, VdpRegs * response) override {
            response->set_v00(VDP_Reg.regs[0]);
            response->set_v01(VDP_Reg.regs[1]);
            response->set_v02(VDP_Reg.regs[2]);
            response->set_v03(VDP_Reg.regs[3]);
            response->set_v04(VDP_Reg.regs[4]);
            response->set_v05(VDP_Reg.regs[5]);
            response->set_v06(VDP_Reg.regs[6]);
            response->set_v07(VDP_Reg.regs[7]);
            response->set_v08(VDP_Reg.regs[8]);
            response->set_v09(VDP_Reg.regs[9]);
            response->set_v0a(VDP_Reg.regs[10]);
            response->set_v0b(VDP_Reg.regs[11]);
            response->set_v0c(VDP_Reg.regs[12]);
            response->set_v0d(VDP_Reg.regs[13]);
            response->set_v0e(VDP_Reg.regs[14]);
            response->set_v0f(VDP_Reg.regs[15]);
            response->set_v10(VDP_Reg.regs[16]);
            response->set_v11(VDP_Reg.regs[17]);
            response->set_v12(VDP_Reg.regs[18]);
            response->set_v13(VDP_Reg.regs[19]);
            response->set_v14(VDP_Reg.regs[20]);
            response->set_v15(VDP_Reg.regs[21]);
            response->set_v16(VDP_Reg.regs[22]);
            response->set_v17(VDP_Reg.regs[23]);

            return Status::OK;
        }
#endif

        Status pause(ServerContext * context, const Empty * request, Empty * response) override {
#ifdef DEBUG_68K
            M68kDW.DebugStop = true;
#else
            Z80DW.DebugStop = true;
#endif

            if (Paused) {
                return Status::OK;
            }

            toggle_pause();

            return Status::OK;
        }

        Status read_memory(ServerContext * context, const MemoryAS * request, MemData * response) override {
            std::string* _return = response->mutable_data();
            _return->clear();

            for (uint32_t i = 0; i < request->size(); ++i) {
#ifdef DEBUG_68K
                if ((request->address() + i >= 0xA00000 && request->address() + i < 0xA10000) && IsHardwareAddressValid((uint32)(request->address() + i))) {
                    // Z80
                    unsigned char value = (unsigned char)(ReadValueAtHardwareAddress((uint32)((request->address() + i) ^ 1), 1) & 0xFF);
                    _return->push_back(value);
                }
                else if (IsHardwareAddressValid((uint32)(request->address() + i))) {
                    unsigned char value = (unsigned char)(ReadValueAtHardwareAddress((uint32)(request->address() + i), 1) & 0xFF);
                    _return->push_back(value);
                }
                else if (request->address() + i >= (BREAKPOINTS_BASE + 0x00000) && request->address() + i < (BREAKPOINTS_BASE + 0x10000)) {
                    // VRAM
                    unsigned int addr = request->address() + i - (BREAKPOINTS_BASE + 0x00000);
                    _return->push_back(VRam[(addr ^ 1) & 0xFFFF]);
                }
                else if (request->address() + i >= (BREAKPOINTS_BASE + 0x10000) && request->address() + i < (BREAKPOINTS_BASE + 0x20000)) {
                    // CRAM
                    unsigned int addr = request->address() + i - (BREAKPOINTS_BASE + 0x10000);
                    _return->push_back(((UINT8*)CRam)[(addr ^ 1) & 0x1FF]);
                }
                else if (request->address() + i >= (BREAKPOINTS_BASE + 0x20000) && request->address() + i < (BREAKPOINTS_BASE + 0x30000)) {
                    // VSRAM
                    unsigned int addr = request->address() + i - (BREAKPOINTS_BASE + 0x20000);
                    _return->push_back(((UINT8*)VSRam)[(addr ^ 1) & 0xFF]);
                }
                else { // else leave the value nil
                    _return->push_back('\x00');
                }
#else
                _return->push_back(Ram_Z80[request->address() + i]);
#endif
            }

            return Status::OK;
        }

        Status resume(ServerContext * context, const Empty * request, Empty * response) override {
#ifdef DEBUG_68K
            M68kDW.DebugStop = false;
#else
            Z80DW.DebugStop = false;
#endif

            if (!Paused) {
                return Status::OK;
            }

            toggle_pause();

            return Status::OK;
        }

        Status set_gp_reg(ServerContext * context, const GpRegValue * request, Empty * response) override {
#ifdef DEBUG_68K
            if (request->index() >= GpRegsEnum::D0 && request->index() <= GpRegsEnum::D7) { // Dx
                main68k_context.dreg[(int)request->index()] = request->value();
            }
            else if (request->index() >= GpRegsEnum::A0 && request->index() <= GpRegsEnum::A7) { // Ax
                switch (request->index()) {
                case GpRegsEnum::A0:
                    main68k_context.areg[0] = request->value();
                    break;
                case GpRegsEnum::A1:
                    main68k_context.areg[1] = request->value();
                    break;
                case GpRegsEnum::A2:
                    main68k_context.areg[2] = request->value();
                    break;
                case GpRegsEnum::A3:
                    main68k_context.areg[3] = request->value();
                    break;
                case GpRegsEnum::A4:
                    main68k_context.areg[4] = request->value();
                    break;
                case GpRegsEnum::A5:
                    main68k_context.areg[5] = request->value();
                    break;
                case GpRegsEnum::A6:
                    main68k_context.areg[6] = request->value();
                    break;
                case GpRegsEnum::A7:
                    main68k_context.areg[7] = request->value();
                    break;
                }
            }
            else {
                switch (request->index()) {
                case GpRegsEnum::SR:
                    main68k_context.sr = request->value() & 0xFFFF;
                    break;
                case GpRegsEnum::SP:
                    main68k_context.areg[7] = request->value();
                    break;
                }
            }
#else
            switch (request->index()) {
            case GpRegsEnum::AF: {
                M_Z80.AF.b.A = (request->value() >> 8) & 0xFF;
                M_Z80.AF.b.F = (request->value() >> 0) & 0xFF;
            } break;
            case GpRegsEnum::BC: M_Z80.BC.w.BC = request->value(); break;
            case GpRegsEnum::DE: M_Z80.DE.w.DE = request->value(); break;
            case GpRegsEnum::HL: M_Z80.HL.w.HL = request->value(); break;

            case GpRegsEnum::IX: M_Z80.IX.w.IX = request->value(); break;
            case GpRegsEnum::IY: M_Z80.IY.w.IY = request->value(); break;

            case GpRegsEnum::A: M_Z80.AF.b.A = request->value(); break;
            case GpRegsEnum::B: M_Z80.BC.b.B = request->value(); break;
            case GpRegsEnum::C: M_Z80.BC.b.C = request->value(); break;
            case GpRegsEnum::D: M_Z80.DE.b.D = request->value(); break;
            case GpRegsEnum::E: M_Z80.DE.b.E = request->value(); break;
            case GpRegsEnum::H: M_Z80.HL.b.H = request->value(); break;
            case GpRegsEnum::L: M_Z80.HL.b.L = request->value(); break;

            case GpRegsEnum::IXH: M_Z80.IX.b.IXH = request->value(); break;
            case GpRegsEnum::IXL: M_Z80.IX.b.IXL = request->value(); break;
            case GpRegsEnum::IYH: M_Z80.IY.b.IYH = request->value(); break;
            case GpRegsEnum::IYL: M_Z80.IY.b.IYL = request->value(); break;


            case GpRegsEnum::AF2: {
                M_Z80.AF2.b.A2 = (request->value() >> 8) & 0xFF;
                M_Z80.AF2.b.F2 = (request->value() >> 0) & 0xFF;
            } break;
            case GpRegsEnum::BC2: M_Z80.BC2.w.BC2 = request->value(); break;
            case GpRegsEnum::DE2: M_Z80.DE2.w.DE2 = request->value(); break;
            case GpRegsEnum::HL2: M_Z80.HL2.w.HL2 = request->value(); break;

            case GpRegsEnum::I: M_Z80.I = request->value(); break;
            case GpRegsEnum::R: M_Z80.R.w.R = request->value(); break;

            case GpRegsEnum::SP: M_Z80.SP.w.SP = request->value(); break;
            case GpRegsEnum::IP: Z80DW.last_pc = request->value(); break;

            case GpRegsEnum::BANK: Bank_Z80 = request->value(); break;
            }
#endif

            return Status::OK;
        }

#ifdef DEBUG_68K
        Status set_vdp_reg(ServerContext * context, const VdpRegValue * request, Empty * response) override {
            if (request->index() >= VdpRegsEnum::V00 && request->index() <= VdpRegsEnum::V17) {
                VDP_Reg.regs[request->index()] = request->value();
            }

            return Status::OK;
        }
#endif

        Status start_emulation(ServerContext * context, const Empty * request, Empty * response) override {
            init_ida_client(atoi(DebugPort) + 1000);

            if (!client) {
                return Status::CANCELLED;
            }

            client->start_event();
#ifdef DEBUG_68K
            M68kDW.changed.clear();
            client->pause_event(main68k_context.pc, M68kDW.changed);
#else
            Z80DW.changed.clear();
            client->pause_event(M_Z80.PC.w.PC, Z80DW.changed);
#endif

            return Status::OK;
        }

        Status step_into(ServerContext * context, const Empty * request, Empty * response) override {
            Paused = 0;
#ifdef DEBUG_68K
            M68kDW.StepInto = 1;
            M68kDW.DebugStop = false;
#else
            Z80DW.StepInto = 1;
            Z80DW.DebugStop = false;
#endif

            return Status::OK;
        }

        Status step_over(ServerContext * context, const Empty * request, Empty * response) override {
            Paused = 0;
#ifdef DEBUG_68K
            M68kDW.DoStepOver();
            M68kDW.DebugStop = false;
#else
            Z80DW.DoStepOver();
            Z80DW.DebugStop = false;
#endif

            return Status::OK;
        }

        Status toggle_breakpoint(ServerContext * context, const DbgBreakpoint * request, Empty * response) override {
#ifdef DEBUG_68K
            for (auto i = M68kDW.Breakpoints.begin(); i != M68kDW.Breakpoints.end(); ++i) {
#else
            for (auto i = Z80DW.Breakpoints.begin(); i != Z80DW.Breakpoints.end(); ++i) {
#endif
                if (request->type() == (BpType)i->type && request->bstart() == i->start) {
#ifdef DEBUG_68K
                    if (request->is_vdp() == i->is_vdp) {
#endif
                        i->enabled = !i->enabled;
                        break;
#ifdef DEBUG_68K
                    }
#endif
                }
            }

            return Status::OK;
            }

        Status update_breakpoint(ServerContext * context, const DbgBreakpoint * request, Empty * response) override {
#ifdef DEBUG_68K
            for (auto i = M68kDW.Breakpoints.begin(); i != M68kDW.Breakpoints.end(); ++i) {
#else
            for (auto i = Z80DW.Breakpoints.begin(); i != Z80DW.Breakpoints.end(); ++i) {
#endif
                if (request->type() == (BpType)i->type && request->bstart() == i->start) {
#ifdef DEBUG_68K
                    if (request->is_vdp() == i->is_vdp) {
#endif
                        i->enabled = request->enabled();
                        i->elang = request->elang();
                        i->condition = request->condition();
                        break;
#ifdef DEBUG_68K
                    }
#endif
                }
            }

            return Status::OK;
            }

        Status write_memory(ServerContext * context, const MemoryAD * request, Empty * response) override {
            for (size_t i = 0; i < request->data().size(); ++i) {
#ifdef DEBUG_68K
                if ((request->address() + i >= 0xA00000 && request->address() + i < 0xA10000) && IsHardwareAddressValid((uint32)(request->address() + i))) { // Z80
                    WriteValueAtHardwareAddress((uint32)((request->address() + i) ^ 1), request->data()[i] & 0xFF, true);
                }
                else if (IsHardwareAddressValid((uint32)(request->address() + i))) {
                    WriteValueAtHardwareAddress((uint32)(request->address() + i), request->data()[i] & 0xFF, true);
                }
                else if (request->address() + i >= (BREAKPOINTS_BASE + 0x00000) && request->address() + i < (BREAKPOINTS_BASE + 0x10000)) { // VRAM
                    unsigned int addr = request->address() + i - (BREAKPOINTS_BASE + 0x00000);
                    VRam[(addr ^ 1) & 0xFFFF] = request->data()[i] & 0xFF;
                }
                else if (request->address() + i >= (BREAKPOINTS_BASE + 0x10000) && request->address() + i < (BREAKPOINTS_BASE + 0x20000)) { // CRAM
                    unsigned int addr = request->address() + i - (BREAKPOINTS_BASE + 0x10000);
                    ((UINT8*)CRam)[(addr ^ 1) & 0x1FF] = request->data()[i] & 0xFF;
                }
                else if (request->address() + i >= (BREAKPOINTS_BASE + 0x20000) && request->address() + i < (BREAKPOINTS_BASE + 0x30000)) { // VSRAM
                    unsigned int addr = request->address() + i - (BREAKPOINTS_BASE + 0x20000);
                    ((UINT8*)VSRam)[(addr ^ 1) & 0xFF] = request->data()[i] & 0xFF;
                }
#else
                Ram_Z80[request->address() + i] = request->data()[i] & 0xFF;
#endif
            }

            return grpc::Status::OK;
        }
	};
}

struct debugger_t::impl
{
	impl() = default;
	~impl()
	{
		if (server)
		{
            server->Shutdown(std::chrono::system_clock::now() + std::chrono::milliseconds(100));
            if (debug_thread.joinable())
            {
                debug_thread.join();
            }
		}
	}

	void init(int in_port)
	{
		if (!server)
		{
            auto&& server_address = std::format("127.0.0.1:{}", in_port);

            ServerBuilder builder;
            builder.AddListeningPort(server_address, InsecureServerCredentials());
            builder.RegisterService(&service);

            server = builder.BuildAndStart();

            debug_thread = std::thread([this]
            {
                server->Wait();
            });
		}
	}
private:
	std::thread debug_thread;
    gx_server_handler service;
    std::unique_ptr<Server> server;
};

debugger_t::debugger_t() : impl_(std::make_unique<impl>())
{
}

debugger_t::~debugger_t() = default;

void debugger_t::init(int in_port)
{
	impl_->init(in_port);
}
