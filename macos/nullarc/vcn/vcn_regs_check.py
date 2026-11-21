#!/usr/bin/env python3
"""Compare src/phxbringup/PhxVcn.h with the upstream Linux register fixture.

PhxVcn.h hard-codes the VCN 4.0.2 register block for Phoenix. A typo there would be a wrong MMIO address in the
kernel, so the values are checked against tests/vcn_linux_regs.txt, which holds the lines copied verbatim from
drivers/gpu/drm/amd/include/asic_reg/vcn/vcn_4_0_0_{offset,sh_mask}.h plus the few constants that live in
vcn_v4_0.c / amdgpu_vcn.h / psp_gfx_if.h / smu_v13_0_4_ppsmc.h.

Usage: tests/vcn_regs_check.py            (exits non-zero on the first mismatch)
"""
import re
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
HEADER = HERE.parent / "PhxVcn.h"
FIXTURE = HERE / "vcn_linux_regs.txt"

# PhxVcn.h constant -> Linux register name (the R in "REG <name> <offset> <base_idx>").
REG_MAP = {
    "kRegPgfsmConfig": "UVD_PGFSM_CONFIG",
    "kRegPgfsmStatus": "UVD_PGFSM_STATUS",
    "kRegPowerStatus": "UVD_POWER_STATUS",
    "kRegUvdStatus": "UVD_STATUS",
    "kRegVcpuCntl": "UVD_VCPU_CNTL",
    "kRegMastintEn": "UVD_MASTINT_EN",
    "kRegLmiCtrl2": "UVD_LMI_CTRL2",
    "kRegSoftReset": "UVD_SOFT_RESET",
    "kRegLmiCtrl": "UVD_LMI_CTRL",
    "kRegScratch1": "UVD_SCRATCH1",
    "kRegDpgLmaCtl": "UVD_DPG_LMA_CTL",
    "kRegDpgLmaData": "UVD_DPG_LMA_DATA",
    "kRegDpgLmaMask": "UVD_DPG_LMA_MASK",
    "kRegCgcGate": "UVD_CGC_GATE",
    "kRegCgcCtrl": "UVD_CGC_CTRL",
    "kRegSuvdCgcGate": "UVD_SUVD_CGC_GATE",
    "kRegSuvdCgcCtrl": "UVD_SUVD_CGC_CTRL",
    "kRegMpcCntl": "UVD_MPC_CNTL",
    "kRegMpcSetMuxA0": "UVD_MPC_SET_MUXA0",
    "kRegMpcSetMuxB0": "UVD_MPC_SET_MUXB0",
    "kRegMpcSetMux": "UVD_MPC_SET_MUX",
    "kRegLmiVcpuCacheBarLo": "UVD_LMI_VCPU_CACHE_64BIT_BAR_LOW",
    "kRegLmiVcpuCacheBarHi": "UVD_LMI_VCPU_CACHE_64BIT_BAR_HIGH",
    "kRegVcpuCacheOffset0": "UVD_VCPU_CACHE_OFFSET0",
    "kRegVcpuCacheSize0": "UVD_VCPU_CACHE_SIZE0",
    "kRegLmiVcpuCache1BarLo": "UVD_LMI_VCPU_CACHE1_64BIT_BAR_LOW",
    "kRegLmiVcpuCache1BarHi": "UVD_LMI_VCPU_CACHE1_64BIT_BAR_HIGH",
    "kRegVcpuCacheOffset1": "UVD_VCPU_CACHE_OFFSET1",
    "kRegVcpuCacheSize1": "UVD_VCPU_CACHE_SIZE1",
    "kRegLmiVcpuCache2BarLo": "UVD_LMI_VCPU_CACHE2_64BIT_BAR_LOW",
    "kRegLmiVcpuCache2BarHi": "UVD_LMI_VCPU_CACHE2_64BIT_BAR_HIGH",
    "kRegVcpuCacheOffset2": "UVD_VCPU_CACHE_OFFSET2",
    "kRegVcpuCacheSize2": "UVD_VCPU_CACHE_SIZE2",
    "kRegLmiVcpuNc0BarLo": "UVD_LMI_VCPU_NC0_64BIT_BAR_LOW",
    "kRegLmiVcpuNc0BarHi": "UVD_LMI_VCPU_NC0_64BIT_BAR_HIGH",
    "kRegVcpuNoncacheOffset0": "UVD_VCPU_NONCACHE_OFFSET0",
    "kRegVcpuNoncacheSize0": "UVD_VCPU_NONCACHE_SIZE0",
    "kRegGfx10AddrConfig": "UVD_GFX10_ADDR_CONFIG",
    "kRegRbArbCtrl": "UVD_RB_ARB_CTRL",
    "kRegRb1DbCtrl": "VCN_RB1_DB_CTRL",
    "kRegRbBaseLo": "UVD_RB_BASE_LO",
    "kRegRbBaseHi": "UVD_RB_BASE_HI",
    "kRegRbSize": "UVD_RB_SIZE",
    "kRegRbEnable": "VCN_RB_ENABLE",
    "kRegRbRptr": "UVD_RB_RPTR",
    "kRegRbWptr": "UVD_RB_WPTR",
}

# PhxVcn.h constant -> MASK line in the fixture.
MASK_MAP = {
    "kVcpuCntlClkEn": "UVD_VCPU_CNTL__CLK_EN_MASK",
    "kVcpuCntlBlkRst": "UVD_VCPU_CNTL__BLK_RST_MASK",
    "kMastintEnVcpuEn": "UVD_MASTINT_EN__VCPU_EN_MASK",
    "kLmiCtrl2StallUmc": "UVD_LMI_CTRL2__STALL_ARB_UMC_MASK",
    "kSoftResetLmi": "UVD_SOFT_RESET__LMI_SOFT_RESET_MASK",
    "kSoftResetLmiUmc": "UVD_SOFT_RESET__LMI_UMC_SOFT_RESET_MASK",
    "kCgcCtrlDynMode": "UVD_CGC_CTRL__DYN_CLOCK_MODE_MASK",
    "kMpcCntlReplMask": "UVD_MPC_CNTL__REPLACEMENT_MODE_MASK",
    "kRbArbCtrlVcpuDis": "UVD_RB_ARB_CTRL__VCPU_DIS_MASK",
    "kRb1DbCtrlEn": "VCN_RB1_DB_CTRL__EN_MASK",
    "kRbEnableRb1": "VCN_RB_ENABLE__RB1_EN_MASK",
    "kPowerStatusMask": "UVD_POWER_STATUS__UVD_POWER_STATUS_MASK",
    "kUvdStatusVcpuRdy": "UVD_STATUS__VCPU_REPORT__SHIFT",   # compared as 1 << shift below
}

# PhxVcn.h constant -> VALUE / LAYOUT line in the fixture.
VALUE_MAP = {
    "kPgfsmStatusMask": "PGFSM_WAIT_MASK",
    "kUvdStatusBusy": "UVD_STATUS__UVD_BUSY",
    "kPgfsmPoweredPg": "UVD_PGFSM_STATUS__UVDM_UVDU_UVDLM_PWR_ON_3_0",
    "kPgfsmPoweredNoPg": "PGFSM_POWERED_NO_PG",
    "kPgfsmConfigGate": "PGFSM_CONFIG_GATE",
    "kPgfsmGated": "PGFSM_GATED",
    "kPgfsmConfigNoPg": "PGFSM_CONFIG_NO_PG",
    "kPgfsmConfigPg": "PGFSM_CONFIG_PG",
    "kPowerStatusKeep": "POWER_STATUS_KEEP",
    "kPowerStatusPgOn": "POWER_STATUS_PG_ON",
    "kPowerStatusTilesOff": "POWER_STATUS_ANTI_HANG_OFF",
    "kMpcSetMuxA": "MPC_SET_MUXA",
    "kMpcSetMuxB": "MPC_SET_MUXB",
    "kMpcSetMux": "MPC_SET_MUX",
    "kPspFwTypeVcn": "PSP_FW_TYPE_VCN",
    "kCmdEnd": "VCN_ENC_CMD_END",
    "kCmdFence": "VCN_ENC_CMD_FENCE",
    "kCmdNop": "VCN_ENC_CMD_NO_OP",
    "kPpsmcResultOk": "PPSMC_RESULT_OK",
    "kPpsmcPowerDownVcn": "PPSMC_POWERDOWN_VCN",
    "kPpsmcPowerUpVcn": "PPSMC_POWERUP_VCN",
    "kSharedPresentFlags": "SHARED_PRESENT_FLAGS",
    "kSharedSq": "SHARED_SQ",
    "kSharedSmuInterface": "SHARED_SMU_INTERFACE",
    "kSharedDrmKey": "SHARED_DRM_KEY",
    "kSharedSize": "SHARED_SIZE",
}


def parse_header(path):
    src = path.read_text()
    consts = {}
    for m in re.finditer(r"constexpr\s+uint32_t\s+(\w+)\s*=\s*([^;]+);", src):
        name, expr = m.group(1), m.group(2)
        expr = re.sub(r"(?<=[0-9a-fA-F])[uU]\b", "", expr).strip()
        for known, value in consts.items():
            expr = re.sub(r"\b%s\b" % known, str(value), expr)
        try:
            value = eval(expr, {"__builtins__": {}}, {})  # digits and | & ~ << only
        except Exception as exc:  # noqa: BLE001 - report and fail
            raise SystemExit("cannot evaluate %s = %s (%s)" % (name, expr, exc))
        consts[name] = value & 0xFFFFFFFF
    return consts


def parse_fixture(path):
    regs, masks, values, layout = {}, {}, {}, {}
    for line in path.read_text().splitlines():
        line = line.split("#", 1)[0].strip()
        if not line:
            continue
        parts = line.split()
        if parts[0] == "REG":
            regs[parts[1]] = (int(parts[2], 16), int(parts[3]))
        elif parts[0] == "MASK":
            masks[parts[1]] = int(parts[2], 16)
        elif parts[0] == "VALUE":
            values[parts[1]] = int(parts[2], 16)
        elif parts[0] == "LAYOUT":
            layout[parts[1]] = int(parts[2])
        else:
            raise SystemExit("unknown fixture line: %s" % line)
    return regs, masks, values, layout


def main():
    consts = parse_header(HEADER)
    regs, masks, values, layout = parse_fixture(FIXTURE)
    errors = []

    def want(name, got, expected):
        if got != expected:
            errors.append("%s = 0x%x, Linux says 0x%x" % (name, got, expected))

    for name, linux in REG_MAP.items():
        if name not in consts:
            errors.append("%s is missing from PhxVcn.h" % name)
            continue
        if linux not in regs:
            errors.append("fixture has no REG %s" % linux)
            continue
        offset, base_idx = regs[linux]
        want(name, consts[name], (0x7800 if base_idx == 0 else 0x7E00) + offset)

    # every kReg* in the header must be covered, so a new register cannot be added without a check
    for name in consts:
        if name.startswith("kReg") and name not in REG_MAP:
            errors.append("%s has no Linux counterpart in REG_MAP" % name)

    for name, linux in MASK_MAP.items():
        if name not in consts:
            errors.append("%s is missing from PhxVcn.h" % name)
            continue
        expected = (1 << masks[linux]) if linux.endswith("__SHIFT") else masks[linux]
        want(name, consts[name], expected)

    for name, linux in VALUE_MAP.items():
        if name not in consts:
            errors.append("%s is missing from PhxVcn.h" % name)
            continue
        expected = layout[linux] if linux in layout else values[linux]
        want(name, consts[name], expected)

    # constants with no Linux counterpart are derived; pin them so an edit is visible
    want("kPgfsmPoweredNoPg", consts.get("kPgfsmPoweredNoPg"), 0)
    want("kPgfsmPoweredPg", consts.get("kPgfsmPoweredPg"), 0x2A2A8AA0)
    want("kMpcCntlReplShift", consts.get("kMpcCntlReplShift"), 3)
    want("kMpcCntlReplMode", consts.get("kMpcCntlReplMode"), 2)
    want("kRb1DbCtrlOffsetShift", consts.get("kRb1DbCtrlOffsetShift"), 2)
    want("kRb1DbCtrlEn", consts.get("kRb1DbCtrlEn"), 0x40000000)
    want("kDbRangeStart", consts.get("kDbRangeStart"), 0x310)
    want("kDbRangeCount", consts.get("kDbRangeCount"), 8)
    want("kVcnDoorbellRange", consts.get("kVcnDoorbellRange"), 0x00080C40)
    want("kDbIndexLinux", consts.get("kDbIndexLinux"), 0x312)
    want("kVcnDoorbellRangeAddr", consts.get("kVcnDoorbellRangeAddr"), 0x01403BCC)
    want("kRsmuIndex", consts.get("kRsmuIndex"), 0x14)
    want("kDoorbellApertureReg", consts.get("kDoorbellApertureReg"), 0xDE0)
    want("kVcnDoorbellFields", consts.get("kVcnDoorbellFields"), 0x001F0FFC)
    want("kRsmuData", consts.get("kRsmuData"), 0x15)
    want("kFwFlagUnifiedQueue", consts.get("kFwFlagUnifiedQueue"), 1 << 2)
    want("kFwFlagDrmKeyInject", consts.get("kFwFlagDrmKeyInject"), 1 << 4)
    want("kFwFlagSmuDpm", consts.get("kFwFlagSmuDpm"), 1 << 11)

    if errors:
        print("VCN register check FAILED:")
        for e in errors:
            print("  " + e)
        return 1
    print("VCN register check OK: %d registers, %d masks, %d values match the Linux 4.0.2 headers"
          % (len(REG_MAP), len(MASK_MAP), len(VALUE_MAP)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
