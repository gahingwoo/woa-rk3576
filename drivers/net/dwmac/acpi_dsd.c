/*++

Module Name:

    acpi_dsd.c

Abstract:

    Reads the DMA tuning that firmware publishes for the GMAC and turns it into
    a DWMAC_DMA_CFG.

    The EDK2 RK3576 port copies mainline's device-tree description of this MAC
    into ACPI verbatim -- same property names, same values:

      Name (_DSD, Package () {
        ToUUID("daffd814-6eba-4d8c-8a91-bc9bbf4aa301"),
        Package () {
          Package () { "snps,mixed-burst", 1 },
          Package () { "snps,tso", 1 },
          Package () { "snps,axi-config", "AXIC" },
          ...
        }
      })

      Name (AXIC, Package () {
        ToUUID("daffd814-6eba-4d8c-8a91-bc9bbf4aa301"),
        Package () {
          Package () { "snps,wr_osr_lmt", 4 },
          Package () { "snps,rd_osr_lmt", 8 },
          Package () { "snps,blen", Package () { 0, 0, 0, 0, 16, 8, 4 } },
        }
      })

    Note that "snps,axi-config" is a *string naming a sibling object* rather than
    a device-tree phandle; that is this firmware's convention (inherited from the
    RK3588 port), so the string is used as the name of the next object to
    evaluate. Nothing else consumes AXIC today -- Linux's stmmac ACPI path does
    not read it -- so this driver is its first consumer.

    Reading these rather than hardcoding them matters because the two trees ship
    separately: the firmware is the thing that knows the board, and a value
    changed there should not need a matching driver rebuild to take effect.

    Mechanism: IOCTL_ACPI_EVAL_METHOD against the PDO, naming "_DSD" and then
    "AXIC". Both are Name objects rather than Methods; acpi.sys evaluates those
    the same way (this is how every driver reads a Name(_CRS, ...)).

    IOCTL_ACPI_GET_DEVICE_SPECIFIC_DATA looks like a better fit -- it fetches one
    named property from a given _DSD UUID section -- but acpiioct.h declares it
    below a banner reading "INTERNAL-ONLY DEFINITION SECTION", so it is not
    something a third-party driver should build on.

    Every property is optional. A firmware that publishes none of this leaves
    Cfg holding the same defaults stmmac would have used for a device tree with
    none of these properties, so the driver degrades to the previous behaviour
    rather than failing.

Environment:

    Kernel mode. PASSIVE_LEVEL, from EvtDevicePrepareHardware.

--*/

#include "dwmac.h"

#include <initguid.h>
#include <acpiioct.h>

//
// The ACPI device-properties UUID -- the one that says "the package after me is
// a list of name/value pairs". Same value as _DSD's daffd814-... in the ASL.
//
static const GUID DwmacDsdPropertiesGuid = {
    0xdaffd814, 0x6eba, 0x4d8c,
    { 0x8a, 0x91, 0xbc, 0x9b, 0xbf, 0x4a, 0xa3, 0x01 }
};

//
// MethodNameAsUlong wants the four name characters in memory order.
//
#define DWMAC_ACPI_NAME(_a, _b, _c, _d)                                       \
    ((ULONG)(_a) | ((ULONG)(_b) << 8) | ((ULONG)(_c) << 16) |                 \
     ((ULONG)(_d) << 24))

#define DWMAC_ACPI_METHOD_DSD   DWMAC_ACPI_NAME('_', 'D', 'S', 'D')

//
// First attempt size for an evaluation result. The _DSD above serialises to a
// few hundred bytes; the retry path handles anything larger.
//
#define DWMAC_ACPI_OUTPUT_INITIAL   1024

//
// A malformed table must not be able to make us allocate without bound.
//
#define DWMAC_ACPI_OUTPUT_MAX       (64 * 1024)

//
// snps,blen carries seven entries, one per burst length the core can be told to
// use. Same as stmmac's AXI_BLEN.
//
#define DWMAC_AXI_BLEN_COUNT        7

//
// FIELD_OFFSET is a signed LONG, and every length it gets compared against here
// is unsigned. Name the two offsets once, already unsigned.
//
#define DWMAC_ACPI_OUTPUT_HEADER                                              \
    ((ULONG)FIELD_OFFSET(ACPI_EVAL_OUTPUT_BUFFER, Argument))

#define DWMAC_ACPI_ARG_HEADER                                                 \
    ((ULONG_PTR)FIELD_OFFSET(ACPI_METHOD_ARGUMENT, Data))

//
// ---------------------------------------------------------------------------
// Bounds-checked walking of an ACPI evaluation result
// ---------------------------------------------------------------------------
//
// Arguments are variable-length and self-describing, which means a table that
// lies about a length can walk us off the end of the buffer. Every step below
// is checked against the end of the region that contains it, so the worst a bad
// table can do is make us give up early.
//

typedef struct _DWMAC_ACPI_PKG {
    PUCHAR Begin;
    PUCHAR End;
} DWMAC_ACPI_PKG;

//
// TRUE if a complete argument -- header plus its data -- starts at Cur and ends
// at or before End.
//
static
BOOLEAN
DwmacArgFits(
    _In_opt_ PUCHAR Cur,
    _In_opt_ PUCHAR End
    )
{
    PACPI_METHOD_ARGUMENT arg;
    ULONG_PTR avail;

    if (Cur == NULL || End == NULL || Cur >= End) {
        return FALSE;
    }

    avail = (ULONG_PTR)(End - Cur);
    if (avail < DWMAC_ACPI_ARG_HEADER) {
        return FALSE;
    }

    arg = (PACPI_METHOD_ARGUMENT)Cur;
    return (BOOLEAN)(avail >= ACPI_METHOD_ARGUMENT_LENGTH_FROM_ARGUMENT(arg));
}

static
PACPI_METHOD_ARGUMENT
DwmacPkgFirst(
    _In_ const DWMAC_ACPI_PKG *Pkg
    )
{
    return DwmacArgFits(Pkg->Begin, Pkg->End)
               ? (PACPI_METHOD_ARGUMENT)Pkg->Begin
               : NULL;
}

static
PACPI_METHOD_ARGUMENT
DwmacPkgNext(
    _In_ const DWMAC_ACPI_PKG *Pkg,
    _In_ PACPI_METHOD_ARGUMENT Arg
    )
{
    PUCHAR next = (PUCHAR)Arg + ACPI_METHOD_ARGUMENT_LENGTH_FROM_ARGUMENT(Arg);

    return DwmacArgFits(next, Pkg->End) ? (PACPI_METHOD_ARGUMENT)next : NULL;
}

//
// Step into a nested package. Its elements are a flat run of arguments inside
// this argument's data.
//
static
BOOLEAN
DwmacArgAsPackage(
    _In_ PACPI_METHOD_ARGUMENT Arg,
    _Out_ DWMAC_ACPI_PKG *Pkg
    )
{
    Pkg->Begin = NULL;
    Pkg->End = NULL;

    if (Arg->Type != ACPI_METHOD_ARGUMENT_PACKAGE &&
        Arg->Type != ACPI_METHOD_ARGUMENT_PACKAGE_EX) {
        return FALSE;
    }

    Pkg->Begin = (PUCHAR)Arg->Data;
    Pkg->End = (PUCHAR)Arg->Data + Arg->DataLength;
    return TRUE;
}

//
// Local rather than the CRT's strlen: these are all short literals, and this
// keeps the file free of any assumption about which string routines the kernel
// build environment exposes.
//
static
SIZE_T
DwmacStrLen(
    _In_z_ PCSTR S
    )
{
    SIZE_T n = 0;

    while (S[n] != '\0') {
        n += 1;
    }
    return n;
}

static
BOOLEAN
DwmacArgIsString(
    _In_ PACPI_METHOD_ARGUMENT Arg,
    _In_z_ PCSTR Name
    )
{
    SIZE_T len = DwmacStrLen(Name);

    if (Arg->Type != ACPI_METHOD_ARGUMENT_STRING) {
        return FALSE;
    }

    //
    // DataLength counts the terminator.
    //
    if ((SIZE_T)Arg->DataLength != len + 1) {
        return FALSE;
    }

    return (BOOLEAN)(RtlCompareMemory(Arg->Data, Name, len) == len);
}

//
// ---------------------------------------------------------------------------
// Evaluating a named object
// ---------------------------------------------------------------------------
//

//
// PASSIVE_LEVEL only -- this waits on the IRP. Returns a pool block the caller
// frees with DWMAC_POOL_TAG, or NULL if the object is absent or unreadable.
//
static
PACPI_EVAL_OUTPUT_BUFFER
DwmacEvalAcpiName(
    _In_ PDEVICE_OBJECT Pdo,
    _In_ ULONG NameAsUlong
    )
{
    ACPI_EVAL_INPUT_BUFFER input;
    PACPI_EVAL_OUTPUT_BUFFER output;
    ULONG size = DWMAC_ACPI_OUTPUT_INITIAL;
    ULONG attempt;

    RtlZeroMemory(&input, sizeof(input));
    input.Signature = ACPI_EVAL_INPUT_BUFFER_SIGNATURE;
    input.MethodNameAsUlong = NameAsUlong;

    //
    // Two attempts: one at the guessed size, and if that was too small, one at
    // the size the ACPI driver reports back in the truncated header.
    //
    for (attempt = 0; attempt < 2; attempt += 1) {
        NTSTATUS status;
        ULONG returned;

        output = (PACPI_EVAL_OUTPUT_BUFFER)ExAllocatePool2(POOL_FLAG_NON_PAGED,
                                                           size,
                                                           DWMAC_POOL_TAG);
        if (output == NULL) {
            return NULL;
        }

        status = DwmacSendAcpiIoctl(Pdo,
                                    &input,
                                    sizeof(input),
                                    output,
                                    size,
                                    &returned);

        if (NT_SUCCESS(status) &&
            returned >= DWMAC_ACPI_OUTPUT_HEADER &&
            output->Signature == ACPI_EVAL_OUTPUT_BUFFER_SIGNATURE &&
            output->Length >= DWMAC_ACPI_OUTPUT_HEADER &&
            output->Length <= returned) {
            return output;
        }

        //
        // Anything other than "too small" is final. So is a second failure, and
        // so is a required size that is absurd or not actually bigger.
        //
        if (status != STATUS_BUFFER_OVERFLOW &&
            status != STATUS_BUFFER_TOO_SMALL) {
            ExFreePoolWithTag(output, DWMAC_POOL_TAG);
            return NULL;
        }

        if (returned < DWMAC_ACPI_OUTPUT_HEADER ||
            output->Length <= size ||
            output->Length > DWMAC_ACPI_OUTPUT_MAX) {
            ExFreePoolWithTag(output, DWMAC_POOL_TAG);
            return NULL;
        }

        size = output->Length;
        ExFreePoolWithTag(output, DWMAC_POOL_TAG);
    }

    return NULL;
}

//
// The whole result, as a package of elements.
//
// A method that returns a package is flattened by the ACPI driver into the
// top-level argument list, which is how the _PSS-style examples read one. We
// have no hardware yet to confirm that a Name holding a package comes back the
// same way rather than as a single nested-package argument, so both shapes are
// accepted: if the result is exactly one package argument, descend into it.
// That costs one comparison and removes the guess.
//
static
BOOLEAN
DwmacResultAsPackage(
    _In_ PACPI_EVAL_OUTPUT_BUFFER Out,
    _Out_ DWMAC_ACPI_PKG *Pkg
    )
{
    PACPI_METHOD_ARGUMENT first;

    Pkg->Begin = (PUCHAR)Out->Argument;
    Pkg->End = (PUCHAR)Out->Argument +
               (Out->Length - DWMAC_ACPI_OUTPUT_HEADER);

    first = DwmacPkgFirst(Pkg);
    if (first == NULL) {
        return FALSE;
    }

    if (Out->Count == 1) {
        DWMAC_ACPI_PKG inner;

        if (DwmacArgAsPackage(first, &inner)) {
            *Pkg = inner;
        }
    }

    return TRUE;
}

//
// A _DSD-shaped object is a sequence of (UUID buffer, properties package)
// pairs. Find the pair whose UUID is the device-properties one and hand back
// its package.
//
static
BOOLEAN
DwmacFindPropertiesPackage(
    _In_ const DWMAC_ACPI_PKG *Root,
    _Out_ DWMAC_ACPI_PKG *Props
    )
{
    PACPI_METHOD_ARGUMENT arg;

    Props->Begin = NULL;
    Props->End = NULL;

    for (arg = DwmacPkgFirst(Root); arg != NULL; arg = DwmacPkgNext(Root, arg)) {
        PACPI_METHOD_ARGUMENT value;

        if (arg->Type != ACPI_METHOD_ARGUMENT_BUFFER ||
            arg->DataLength != sizeof(GUID) ||
            RtlCompareMemory(arg->Data,
                             &DwmacDsdPropertiesGuid,
                             sizeof(GUID)) != sizeof(GUID)) {
            continue;
        }

        value = DwmacPkgNext(Root, arg);
        if (value != NULL && DwmacArgAsPackage(value, Props)) {
            return TRUE;
        }
    }

    return FALSE;
}

//
// Properties are Package () { name, value } pairs. Returns the value.
//
static
PACPI_METHOD_ARGUMENT
DwmacFindProperty(
    _In_ const DWMAC_ACPI_PKG *Props,
    _In_z_ PCSTR Name
    )
{
    PACPI_METHOD_ARGUMENT entry;

    for (entry = DwmacPkgFirst(Props);
         entry != NULL;
         entry = DwmacPkgNext(Props, entry)) {
        DWMAC_ACPI_PKG pair;
        PACPI_METHOD_ARGUMENT key;
        PACPI_METHOD_ARGUMENT value;

        if (!DwmacArgAsPackage(entry, &pair)) {
            continue;
        }

        key = DwmacPkgFirst(&pair);
        if (key == NULL || !DwmacArgIsString(key, Name)) {
            continue;
        }

        value = DwmacPkgNext(&pair, key);
        if (value != NULL) {
            return value;
        }
    }

    return NULL;
}

static
BOOLEAN
DwmacGetIntegerProperty(
    _In_ const DWMAC_ACPI_PKG *Props,
    _In_z_ PCSTR Name,
    _Out_ PULONG Value
    )
{
    PACPI_METHOD_ARGUMENT arg = DwmacFindProperty(Props, Name);

    *Value = 0;

    if (arg == NULL || arg->Type != ACPI_METHOD_ARGUMENT_INTEGER) {
        return FALSE;
    }

    *Value = arg->Argument;
    return TRUE;
}

//
// Device-tree booleans are "present or absent". The ASL spells them as an
// integer, so treat a present non-zero integer as set -- and a bare present
// property with no usable value as set too, which is the device-tree reading.
//
static
BOOLEAN
DwmacGetBooleanProperty(
    _In_ const DWMAC_ACPI_PKG *Props,
    _In_z_ PCSTR Name,
    _In_ BOOLEAN Default
    )
{
    PACPI_METHOD_ARGUMENT arg = DwmacFindProperty(Props, Name);

    if (arg == NULL) {
        return Default;
    }

    if (arg->Type == ACPI_METHOD_ARGUMENT_INTEGER) {
        return (BOOLEAN)(arg->Argument != 0);
    }

    return TRUE;
}

//
// ---------------------------------------------------------------------------
// snps,blen -> the BLEN field of DMA_SYS_BUS_MODE
// ---------------------------------------------------------------------------
//
// The array lists burst lengths to permit; zero entries are skipped. Valid
// lengths are the powers of two from 4 to 256, and the register field starts at
// 4, so burst >> 2 is the bit to set. This mirrors stmmac_axi_blen_to_mask().
//

static
ULONG
DwmacBlenToMask(
    _In_ const DWMAC_ACPI_PKG *Blen
    )
{
    PACPI_METHOD_ARGUMENT arg;
    ULONG mask = 0;
    ULONG count = 0;

    for (arg = DwmacPkgFirst(Blen);
         arg != NULL && count < DWMAC_AXI_BLEN_COUNT;
         arg = DwmacPkgNext(Blen, arg), count += 1) {
        ULONG burst;

        if (arg->Type != ACPI_METHOD_ARGUMENT_INTEGER) {
            continue;
        }

        burst = arg->Argument;
        if (burst == 0) {
            continue;
        }

        //
        // Power of two, in range. Anything else is a firmware bug; drop it
        // rather than shifting a garbage value into the register.
        //
        if (burst < 4 || burst > 256 || (burst & (burst - 1)) != 0) {
            RkLog(RK_DBG_ERROR, "_DSD: ignoring invalid snps,blen entry %lu\n",
                  burst);
            continue;
        }

        mask |= burst >> 2;
    }

    return (mask << 1) & DMA_SYS_BUS_BLEN_MASK;
}

//
// ---------------------------------------------------------------------------
// snps,axi-config -> the AXIC package
// ---------------------------------------------------------------------------
//

static
VOID
DwmacReadAxiConfig(
    _In_ PDEVICE_OBJECT Pdo,
    _In_ const DWMAC_ACPI_PKG *Props,
    _Inout_ PDWMAC_DMA_CFG Cfg
    )
{
    PACPI_METHOD_ARGUMENT nameArg;
    PACPI_EVAL_OUTPUT_BUFFER out;
    DWMAC_ACPI_PKG root;
    DWMAC_ACPI_PKG axi;
    PACPI_METHOD_ARGUMENT blenArg;
    CHAR name[5];
    ULONG nameAsUlong;
    ULONG value;

    //
    // The property value is the four-character name of a sibling object.
    // Anything that is not exactly four characters cannot be an ACPI name.
    //
    nameArg = DwmacFindProperty(Props, "snps,axi-config");
    if (nameArg == NULL) {
        return;
    }

    if (nameArg->Type != ACPI_METHOD_ARGUMENT_STRING ||
        nameArg->DataLength != 5 ||
        nameArg->Data[4] != '\0') {
        RkLog(RK_DBG_ERROR, "_DSD: snps,axi-config is not a 4-character name\n");
        return;
    }

    name[0] = (CHAR)nameArg->Data[0];
    name[1] = (CHAR)nameArg->Data[1];
    name[2] = (CHAR)nameArg->Data[2];
    name[3] = (CHAR)nameArg->Data[3];
    name[4] = '\0';

    nameAsUlong = DWMAC_ACPI_NAME(nameArg->Data[0], nameArg->Data[1],
                                  nameArg->Data[2], nameArg->Data[3]);

    out = DwmacEvalAcpiName(Pdo, nameAsUlong);
    if (out == NULL) {
        RkLog(RK_DBG_ERROR, "_DSD: snps,axi-config names %s, which did not "
                            "evaluate; leaving the AXI fields at reset\n",
              name);
        return;
    }

    if (!DwmacResultAsPackage(out, &root) ||
        !DwmacFindPropertiesPackage(&root, &axi)) {
        ExFreePoolWithTag(out, DWMAC_POOL_TAG);
        return;
    }

    Cfg->AxiConfigFound = TRUE;

    Cfg->AxiLpiEn = DwmacGetBooleanProperty(&axi, "snps,lpi_en", FALSE);
    Cfg->AxiXitFrm = DwmacGetBooleanProperty(&axi, "snps,xit_frm", FALSE);

    if (DwmacGetIntegerProperty(&axi, "snps,wr_osr_lmt", &value)) {
        Cfg->AxiWrOsrLmt = value;
    }
    if (DwmacGetIntegerProperty(&axi, "snps,rd_osr_lmt", &value)) {
        Cfg->AxiRdOsrLmt = value;
    }

    //
    // The register fields are four bits wide and hold "outstanding requests
    // minus one". A firmware asking for more than the field can express would
    // otherwise silently wrap.
    //
    if (Cfg->AxiWrOsrLmt > 15 || Cfg->AxiRdOsrLmt > 15) {
        RkLog(RK_DBG_ERROR, "AXIC: osr_lmt out of range (wr %lu rd %lu); "
                            "clamping to 15\n",
              Cfg->AxiWrOsrLmt, Cfg->AxiRdOsrLmt);
        Cfg->AxiWrOsrLmt = min(Cfg->AxiWrOsrLmt, 15u);
        Cfg->AxiRdOsrLmt = min(Cfg->AxiRdOsrLmt, 15u);
    }

    blenArg = DwmacFindProperty(&axi, "snps,blen");
    if (blenArg != NULL) {
        DWMAC_ACPI_PKG blen;

        if (DwmacArgAsPackage(blenArg, &blen)) {
            Cfg->AxiBlenMask = DwmacBlenToMask(&blen);
        }
    }

    ExFreePoolWithTag(out, DWMAC_POOL_TAG);
}

//
// ---------------------------------------------------------------------------
// Enhanced addressing
// ---------------------------------------------------------------------------
//
// EAME is not a firmware property in the device tree either -- stmmac turns it
// on whenever the core reports it can emit addresses wider than 32 bits and the
// kernel is using 64-bit DMA addresses. This driver's DMA enabler is
// WdfDmaProfilePacket64 and it writes the _HI descriptor registers and the
// upper half of each buffer address, so without EAME the core would quietly
// drop those upper bits.
//
// That matters here specifically: RK3576 DRAM starts at 0x40000000 and a 4 GB
// board runs to 0x140000000, so a common buffer really can land above 4 GB.
//

static
BOOLEAN
DwmacCoreHasWideDma(
    _In_ PDWMAC_ADAPTER A
    )
{
    ULONG feature1 = DwmacRead(A->Mac, GMAC_HW_FEATURE1);
    ULONG addr64 = (feature1 & GMAC_HW_FEATURE1_ADDR64_MASK) >>
                   GMAC_HW_FEATURE1_ADDR64_SHIFT;

    //
    // 0 = 32-bit, 1 = 40-bit, 2 = 48-bit. Anything else is reserved; treat it
    // as 32-bit rather than assume.
    //
    return (BOOLEAN)(addr64 == 1 || addr64 == 2);
}

//
// ---------------------------------------------------------------------------
// Entry point
// ---------------------------------------------------------------------------
//

_Use_decl_annotations_
VOID
DwmacReadDmaCfgFromFirmware(
    PDWMAC_ADAPTER A,
    PDWMAC_DMA_CFG Cfg
    )
{
    PDEVICE_OBJECT pdo;
    PACPI_EVAL_OUTPUT_BUFFER out;
    DWMAC_ACPI_PKG root;
    DWMAC_ACPI_PKG props;
    ULONG value;

    //
    // stmmac's defaults for a device tree that says nothing.
    //
    RtlZeroMemory(Cfg, sizeof(*Cfg));
    Cfg->Pbl = DWMAC_DEFAULT_PBL;
    Cfg->PblX8 = TRUE;
    Cfg->AxiWrOsrLmt = 1;
    Cfg->AxiRdOsrLmt = 1;

    Cfg->Eame = DwmacCoreHasWideDma(A);

    pdo = WdfDeviceWdmGetPhysicalDevice(A->Device);
    if (pdo == NULL) {
        return;
    }

    out = DwmacEvalAcpiName(pdo, DWMAC_ACPI_METHOD_DSD);
    if (out == NULL) {
        RkLog(RK_DBG_INFO, "no _DSD; using default DMA tuning\n");
        return;
    }

    if (!DwmacResultAsPackage(out, &root) ||
        !DwmacFindPropertiesPackage(&root, &props)) {
        RkLog(RK_DBG_ERROR, "_DSD has no device-properties package; using "
                            "default DMA tuning\n");
        ExFreePoolWithTag(out, DWMAC_POOL_TAG);
        return;
    }

    if (DwmacGetIntegerProperty(&props, "snps,pbl", &value) && value != 0) {
        Cfg->Pbl = value;
    }
    (VOID)DwmacGetIntegerProperty(&props, "snps,txpbl", &Cfg->TxPbl);
    (VOID)DwmacGetIntegerProperty(&props, "snps,rxpbl", &Cfg->RxPbl);

    Cfg->PblX8 = DwmacGetBooleanProperty(&props, "snps,no-pbl-x8", FALSE)
                     ? FALSE
                     : TRUE;

    Cfg->Aal = DwmacGetBooleanProperty(&props, "snps,aal", FALSE);
    Cfg->FixedBurst = DwmacGetBooleanProperty(&props, "snps,fixed-burst", FALSE);
    Cfg->MixedBurst = DwmacGetBooleanProperty(&props, "snps,mixed-burst", FALSE);

    //
    // The PBL field is six bits wide, and PBLx8 multiplies it. Clamp rather
    // than let a bad value truncate into something unrelated.
    //
    if (Cfg->Pbl > 63 || Cfg->TxPbl > 63 || Cfg->RxPbl > 63) {
        RkLog(RK_DBG_ERROR, "_DSD: pbl out of range (%lu/%lu/%lu); clamping\n",
              Cfg->Pbl, Cfg->TxPbl, Cfg->RxPbl);
        Cfg->Pbl = min(Cfg->Pbl, 63u);
        Cfg->TxPbl = min(Cfg->TxPbl, 63u);
        Cfg->RxPbl = min(Cfg->RxPbl, 63u);
    }

    DwmacReadAxiConfig(pdo, &props, Cfg);

    ExFreePoolWithTag(out, DWMAC_POOL_TAG);

    RkLog(RK_DBG_INFO,
          "DMA tuning: pbl %lu (tx %lu rx %lu) x8 %lu, fb %lu mb %lu aal %lu "
          "eame %lu\n",
          Cfg->Pbl, Cfg->TxPbl, Cfg->RxPbl, (ULONG)Cfg->PblX8,
          (ULONG)Cfg->FixedBurst, (ULONG)Cfg->MixedBurst, (ULONG)Cfg->Aal,
          (ULONG)Cfg->Eame);

    if (Cfg->AxiConfigFound) {
        RkLog(RK_DBG_INFO, "AXI: wr_osr %lu rd_osr %lu blen 0x%02lx\n",
              Cfg->AxiWrOsrLmt, Cfg->AxiRdOsrLmt, Cfg->AxiBlenMask);
    } else {
        RkLog(RK_DBG_INFO, "AXI: no axi-config; leaving those fields at reset\n");
    }
}
