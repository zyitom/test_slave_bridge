/*
 * RMCS stream-bridge object dictionary.
 *
 * INSTALLED OVER the SSC-Tool-generated digital_ioObjects.h by
 * ecat/tools/import_ssc.sh (the generated stack file digital_io.h includes it
 * by this exact name, hence the name mismatch with its content). Replacing
 * the generated header keeps every project-specific object definition under
 * version control while the license-gated Beckhoff sources stay local-only:
 * the SSC Tool project can remain the stock SDK ecat_io configuration and
 * never needs to be edited when the process data layout changes here.
 *
 * Layout: one 128-byte stream chunk per direction (see common/pd_stream.hpp),
 * described to CoE as arrays of 32 UNSIGNED32 because a single PDO-mapping
 * entry cannot exceed 255 bits (8-bit length field, ETG.1000.6). The mapping
 * is fixed: 0x1A00 maps 0x6000:01..:20 (inputs, slave -> master), 0x1600 maps
 * 0x7010:01..:20 (outputs, master -> slave). Sizes must agree with
 * RMCS_PD_CHUNK_SIZE (rmcs_pd.h) and MAX_PD_*_SIZE (core0/CMakeLists.txt);
 * ecat_appl.c static-asserts the arithmetic.
 *
 * The object variables below are CoE/SDO-visible placeholders only: the
 * cyclic stream bytes are moved directly between the ESC process data image
 * and the cross-core rings by APPL_InputMapping()/APPL_OutputMapping(), never
 * through these variables.
 */

/* Marker checked by core0/CMakeLists.txt to reject a stock generated file. */
#define RMCS_STREAM_OBJECTS 1

#if defined(_DIGITAL_IO_) && (_DIGITAL_IO_ == 1)
#define PROTO
#else
#define PROTO extern
#endif

/* Mapping entry (index << 16) | (subindex << 8) | bit length. */
#define RMCS_MAP_ENTRY(index, subindex) (((UINT32)(index) << 16) | ((subindex) << 8) | 0x20U)

#define RMCS_STREAM_ENTRY_COUNT 32 /* 32 x UNSIGNED32 = 128 bytes */

/******************************************************************************
 *                    Object 0x1600 : OutputStream process data mapping
 ******************************************************************************/
#ifdef _OBJD_
OBJCONST TSDOINFOENTRYDESC OBJMEM asEntryDesc0x1600[] = {
    {DEFTYPE_UNSIGNED8, 0x8, ACCESS_READ},
    {DEFTYPE_UNSIGNED32, 0x20, ACCESS_READ}, {DEFTYPE_UNSIGNED32, 0x20, ACCESS_READ},
    {DEFTYPE_UNSIGNED32, 0x20, ACCESS_READ}, {DEFTYPE_UNSIGNED32, 0x20, ACCESS_READ},
    {DEFTYPE_UNSIGNED32, 0x20, ACCESS_READ}, {DEFTYPE_UNSIGNED32, 0x20, ACCESS_READ},
    {DEFTYPE_UNSIGNED32, 0x20, ACCESS_READ}, {DEFTYPE_UNSIGNED32, 0x20, ACCESS_READ},
    {DEFTYPE_UNSIGNED32, 0x20, ACCESS_READ}, {DEFTYPE_UNSIGNED32, 0x20, ACCESS_READ},
    {DEFTYPE_UNSIGNED32, 0x20, ACCESS_READ}, {DEFTYPE_UNSIGNED32, 0x20, ACCESS_READ},
    {DEFTYPE_UNSIGNED32, 0x20, ACCESS_READ}, {DEFTYPE_UNSIGNED32, 0x20, ACCESS_READ},
    {DEFTYPE_UNSIGNED32, 0x20, ACCESS_READ}, {DEFTYPE_UNSIGNED32, 0x20, ACCESS_READ},
    {DEFTYPE_UNSIGNED32, 0x20, ACCESS_READ}, {DEFTYPE_UNSIGNED32, 0x20, ACCESS_READ},
    {DEFTYPE_UNSIGNED32, 0x20, ACCESS_READ}, {DEFTYPE_UNSIGNED32, 0x20, ACCESS_READ},
    {DEFTYPE_UNSIGNED32, 0x20, ACCESS_READ}, {DEFTYPE_UNSIGNED32, 0x20, ACCESS_READ},
    {DEFTYPE_UNSIGNED32, 0x20, ACCESS_READ}, {DEFTYPE_UNSIGNED32, 0x20, ACCESS_READ},
    {DEFTYPE_UNSIGNED32, 0x20, ACCESS_READ}, {DEFTYPE_UNSIGNED32, 0x20, ACCESS_READ},
    {DEFTYPE_UNSIGNED32, 0x20, ACCESS_READ}, {DEFTYPE_UNSIGNED32, 0x20, ACCESS_READ},
    {DEFTYPE_UNSIGNED32, 0x20, ACCESS_READ}, {DEFTYPE_UNSIGNED32, 0x20, ACCESS_READ},
    {DEFTYPE_UNSIGNED32, 0x20, ACCESS_READ}, {DEFTYPE_UNSIGNED32, 0x20, ACCESS_READ}};

OBJCONST UCHAR OBJMEM aName0x1600[] = "OutputStream process data mapping\000\377";
#endif /* _OBJD_ */

#ifndef _DIGITAL_IO_OBJECTS_H_
typedef struct OBJ_STRUCT_PACKED_START {
    UINT16 u16SubIndex0;
    UINT32 aEntries[RMCS_STREAM_ENTRY_COUNT];
} OBJ_STRUCT_PACKED_END TOBJ1600;
#endif /* _DIGITAL_IO_OBJECTS_H_ */

PROTO TOBJ1600 sOutputStreamMapping0x1600
#if defined(_DIGITAL_IO_) && (_DIGITAL_IO_ == 1)
    = {RMCS_STREAM_ENTRY_COUNT,
       {RMCS_MAP_ENTRY(0x7010, 1),  RMCS_MAP_ENTRY(0x7010, 2),  RMCS_MAP_ENTRY(0x7010, 3),
        RMCS_MAP_ENTRY(0x7010, 4),  RMCS_MAP_ENTRY(0x7010, 5),  RMCS_MAP_ENTRY(0x7010, 6),
        RMCS_MAP_ENTRY(0x7010, 7),  RMCS_MAP_ENTRY(0x7010, 8),  RMCS_MAP_ENTRY(0x7010, 9),
        RMCS_MAP_ENTRY(0x7010, 10), RMCS_MAP_ENTRY(0x7010, 11), RMCS_MAP_ENTRY(0x7010, 12),
        RMCS_MAP_ENTRY(0x7010, 13), RMCS_MAP_ENTRY(0x7010, 14), RMCS_MAP_ENTRY(0x7010, 15),
        RMCS_MAP_ENTRY(0x7010, 16), RMCS_MAP_ENTRY(0x7010, 17), RMCS_MAP_ENTRY(0x7010, 18),
        RMCS_MAP_ENTRY(0x7010, 19), RMCS_MAP_ENTRY(0x7010, 20), RMCS_MAP_ENTRY(0x7010, 21),
        RMCS_MAP_ENTRY(0x7010, 22), RMCS_MAP_ENTRY(0x7010, 23), RMCS_MAP_ENTRY(0x7010, 24),
        RMCS_MAP_ENTRY(0x7010, 25), RMCS_MAP_ENTRY(0x7010, 26), RMCS_MAP_ENTRY(0x7010, 27),
        RMCS_MAP_ENTRY(0x7010, 28), RMCS_MAP_ENTRY(0x7010, 29), RMCS_MAP_ENTRY(0x7010, 30),
        RMCS_MAP_ENTRY(0x7010, 31), RMCS_MAP_ENTRY(0x7010, 32)}}
#endif
;

/******************************************************************************
 *                    Object 0x1A00 : InputStream process data mapping
 ******************************************************************************/
#ifdef _OBJD_
OBJCONST TSDOINFOENTRYDESC OBJMEM asEntryDesc0x1A00[] = {
    {DEFTYPE_UNSIGNED8, 0x8, ACCESS_READ},
    {DEFTYPE_UNSIGNED32, 0x20, ACCESS_READ}, {DEFTYPE_UNSIGNED32, 0x20, ACCESS_READ},
    {DEFTYPE_UNSIGNED32, 0x20, ACCESS_READ}, {DEFTYPE_UNSIGNED32, 0x20, ACCESS_READ},
    {DEFTYPE_UNSIGNED32, 0x20, ACCESS_READ}, {DEFTYPE_UNSIGNED32, 0x20, ACCESS_READ},
    {DEFTYPE_UNSIGNED32, 0x20, ACCESS_READ}, {DEFTYPE_UNSIGNED32, 0x20, ACCESS_READ},
    {DEFTYPE_UNSIGNED32, 0x20, ACCESS_READ}, {DEFTYPE_UNSIGNED32, 0x20, ACCESS_READ},
    {DEFTYPE_UNSIGNED32, 0x20, ACCESS_READ}, {DEFTYPE_UNSIGNED32, 0x20, ACCESS_READ},
    {DEFTYPE_UNSIGNED32, 0x20, ACCESS_READ}, {DEFTYPE_UNSIGNED32, 0x20, ACCESS_READ},
    {DEFTYPE_UNSIGNED32, 0x20, ACCESS_READ}, {DEFTYPE_UNSIGNED32, 0x20, ACCESS_READ},
    {DEFTYPE_UNSIGNED32, 0x20, ACCESS_READ}, {DEFTYPE_UNSIGNED32, 0x20, ACCESS_READ},
    {DEFTYPE_UNSIGNED32, 0x20, ACCESS_READ}, {DEFTYPE_UNSIGNED32, 0x20, ACCESS_READ},
    {DEFTYPE_UNSIGNED32, 0x20, ACCESS_READ}, {DEFTYPE_UNSIGNED32, 0x20, ACCESS_READ},
    {DEFTYPE_UNSIGNED32, 0x20, ACCESS_READ}, {DEFTYPE_UNSIGNED32, 0x20, ACCESS_READ},
    {DEFTYPE_UNSIGNED32, 0x20, ACCESS_READ}, {DEFTYPE_UNSIGNED32, 0x20, ACCESS_READ},
    {DEFTYPE_UNSIGNED32, 0x20, ACCESS_READ}, {DEFTYPE_UNSIGNED32, 0x20, ACCESS_READ},
    {DEFTYPE_UNSIGNED32, 0x20, ACCESS_READ}, {DEFTYPE_UNSIGNED32, 0x20, ACCESS_READ},
    {DEFTYPE_UNSIGNED32, 0x20, ACCESS_READ}, {DEFTYPE_UNSIGNED32, 0x20, ACCESS_READ}};

OBJCONST UCHAR OBJMEM aName0x1A00[] = "InputStream process data mapping\000\377";
#endif /* _OBJD_ */

#ifndef _DIGITAL_IO_OBJECTS_H_
typedef struct OBJ_STRUCT_PACKED_START {
    UINT16 u16SubIndex0;
    UINT32 aEntries[RMCS_STREAM_ENTRY_COUNT];
} OBJ_STRUCT_PACKED_END TOBJ1A00;
#endif /* _DIGITAL_IO_OBJECTS_H_ */

PROTO TOBJ1A00 sInputStreamMapping0x1A00
#if defined(_DIGITAL_IO_) && (_DIGITAL_IO_ == 1)
    = {RMCS_STREAM_ENTRY_COUNT,
       {RMCS_MAP_ENTRY(0x6000, 1),  RMCS_MAP_ENTRY(0x6000, 2),  RMCS_MAP_ENTRY(0x6000, 3),
        RMCS_MAP_ENTRY(0x6000, 4),  RMCS_MAP_ENTRY(0x6000, 5),  RMCS_MAP_ENTRY(0x6000, 6),
        RMCS_MAP_ENTRY(0x6000, 7),  RMCS_MAP_ENTRY(0x6000, 8),  RMCS_MAP_ENTRY(0x6000, 9),
        RMCS_MAP_ENTRY(0x6000, 10), RMCS_MAP_ENTRY(0x6000, 11), RMCS_MAP_ENTRY(0x6000, 12),
        RMCS_MAP_ENTRY(0x6000, 13), RMCS_MAP_ENTRY(0x6000, 14), RMCS_MAP_ENTRY(0x6000, 15),
        RMCS_MAP_ENTRY(0x6000, 16), RMCS_MAP_ENTRY(0x6000, 17), RMCS_MAP_ENTRY(0x6000, 18),
        RMCS_MAP_ENTRY(0x6000, 19), RMCS_MAP_ENTRY(0x6000, 20), RMCS_MAP_ENTRY(0x6000, 21),
        RMCS_MAP_ENTRY(0x6000, 22), RMCS_MAP_ENTRY(0x6000, 23), RMCS_MAP_ENTRY(0x6000, 24),
        RMCS_MAP_ENTRY(0x6000, 25), RMCS_MAP_ENTRY(0x6000, 26), RMCS_MAP_ENTRY(0x6000, 27),
        RMCS_MAP_ENTRY(0x6000, 28), RMCS_MAP_ENTRY(0x6000, 29), RMCS_MAP_ENTRY(0x6000, 30),
        RMCS_MAP_ENTRY(0x6000, 31), RMCS_MAP_ENTRY(0x6000, 32)}}
#endif
;

/******************************************************************************
 *                    Object 0x1C12 : SyncManager 2 assignment
 ******************************************************************************/
#ifdef _OBJD_
OBJCONST TSDOINFOENTRYDESC OBJMEM asEntryDesc0x1C12[] = {
    {DEFTYPE_UNSIGNED8, 0x8, ACCESS_READ},
    {DEFTYPE_UNSIGNED16, 0x10, ACCESS_READ}};

OBJCONST UCHAR OBJMEM aName0x1C12[] = "SyncManager 2 assignment\000\377";
#endif /* _OBJD_ */

#ifndef _DIGITAL_IO_OBJECTS_H_
typedef struct OBJ_STRUCT_PACKED_START {
    UINT16 u16SubIndex0;
    UINT16 aEntries[1];
} OBJ_STRUCT_PACKED_END TOBJ1C12;
#endif /* _DIGITAL_IO_OBJECTS_H_ */

PROTO TOBJ1C12 sRxPDOassign
#if defined(_DIGITAL_IO_) && (_DIGITAL_IO_ == 1)
    = {1, {0x1600}}
#endif
;

/******************************************************************************
 *                    Object 0x1C13 : SyncManager 3 assignment
 ******************************************************************************/
#ifdef _OBJD_
OBJCONST TSDOINFOENTRYDESC OBJMEM asEntryDesc0x1C13[] = {
    {DEFTYPE_UNSIGNED8, 0x8, ACCESS_READ},
    {DEFTYPE_UNSIGNED16, 0x10, ACCESS_READ}};

OBJCONST UCHAR OBJMEM aName0x1C13[] = "SyncManager 3 assignment\000\377";
#endif /* _OBJD_ */

#ifndef _DIGITAL_IO_OBJECTS_H_
typedef struct OBJ_STRUCT_PACKED_START {
    UINT16 u16SubIndex0;
    UINT16 aEntries[1];
} OBJ_STRUCT_PACKED_END TOBJ1C13;
#endif /* _DIGITAL_IO_OBJECTS_H_ */

PROTO TOBJ1C13 sTxPDOassign
#if defined(_DIGITAL_IO_) && (_DIGITAL_IO_ == 1)
    = {1, {0x1A00}}
#endif
;

/******************************************************************************
 *                    Object 0x6000 : InputStream (slave -> master)
 ******************************************************************************/
#ifdef _OBJD_
/* Array object: entry description [0] is subindex 0, [1] is shared by all
 * data subindexes. */
OBJCONST TSDOINFOENTRYDESC OBJMEM asEntryDesc0x6000[] = {
    {DEFTYPE_UNSIGNED8, 0x8, ACCESS_READ},
    {DEFTYPE_UNSIGNED32, 0x20, ACCESS_READ | OBJACCESS_TXPDOMAPPING}};

OBJCONST UCHAR OBJMEM aName0x6000[] = "InputStream\000\377";
#endif /* _OBJD_ */

#ifndef _DIGITAL_IO_OBJECTS_H_
typedef struct OBJ_STRUCT_PACKED_START {
    UINT16 u16SubIndex0;
    UINT32 aData[RMCS_STREAM_ENTRY_COUNT];
} OBJ_STRUCT_PACKED_END TOBJ6000;
#endif /* _DIGITAL_IO_OBJECTS_H_ */

PROTO TOBJ6000 sInputStream0x6000
#if defined(_DIGITAL_IO_) && (_DIGITAL_IO_ == 1)
    = {RMCS_STREAM_ENTRY_COUNT, {0}}
#endif
;

/******************************************************************************
 *                    Object 0x7010 : OutputStream (master -> slave)
 ******************************************************************************/
#ifdef _OBJD_
OBJCONST TSDOINFOENTRYDESC OBJMEM asEntryDesc0x7010[] = {
    {DEFTYPE_UNSIGNED8, 0x8, ACCESS_READ},
    {DEFTYPE_UNSIGNED32, 0x20, ACCESS_READWRITE | OBJACCESS_RXPDOMAPPING}};

OBJCONST UCHAR OBJMEM aName0x7010[] = "OutputStream\000\377";
#endif /* _OBJD_ */

#ifndef _DIGITAL_IO_OBJECTS_H_
typedef struct OBJ_STRUCT_PACKED_START {
    UINT16 u16SubIndex0;
    UINT32 aData[RMCS_STREAM_ENTRY_COUNT];
} OBJ_STRUCT_PACKED_END TOBJ7010;
#endif /* _DIGITAL_IO_OBJECTS_H_ */

PROTO TOBJ7010 sOutputStream0x7010
#if defined(_DIGITAL_IO_) && (_DIGITAL_IO_ == 1)
    = {RMCS_STREAM_ENTRY_COUNT, {0}}
#endif
;

/******************************************************************************
 *                    Object 0xF000 : Modular Device Profile
 ******************************************************************************/
#ifdef _OBJD_
OBJCONST TSDOINFOENTRYDESC OBJMEM asEntryDesc0xF000[] = {
    {DEFTYPE_UNSIGNED8, 0x8, ACCESS_READ},
    {DEFTYPE_UNSIGNED16, 0x10, ACCESS_READ},
    {DEFTYPE_UNSIGNED16, 0x10, ACCESS_READ}};

OBJCONST UCHAR OBJMEM aName0xF000[] = "Modular Device Profile\000"
                                      "Index distance \000"
                                      "Maximum number of modules \000\377";
#endif /* _OBJD_ */

#ifndef _DIGITAL_IO_OBJECTS_H_
typedef struct OBJ_STRUCT_PACKED_START {
    UINT16 u16SubIndex0;
    UINT16 IndexDistance;
    UINT16 MaximumNumberOfModules;
} OBJ_STRUCT_PACKED_END TOBJF000;
#endif /* _DIGITAL_IO_OBJECTS_H_ */

PROTO TOBJF000 sModularDeviceProfile0xF000
#if defined(_DIGITAL_IO_) && (_DIGITAL_IO_ == 1)
    = {2, 0x0010, 0}
#endif
;

#ifdef _OBJD_
TOBJECT OBJMEM ApplicationObjDic[] = {
    /* Object 0x1600 */
    {NULL, NULL, 0x1600, {DEFTYPE_PDOMAPPING, RMCS_STREAM_ENTRY_COUNT | (OBJCODE_REC << 8)},
     asEntryDesc0x1600, aName0x1600, &sOutputStreamMapping0x1600, NULL, NULL, 0x0000},
    /* Object 0x1A00 */
    {NULL, NULL, 0x1A00, {DEFTYPE_PDOMAPPING, RMCS_STREAM_ENTRY_COUNT | (OBJCODE_REC << 8)},
     asEntryDesc0x1A00, aName0x1A00, &sInputStreamMapping0x1A00, NULL, NULL, 0x0000},
    /* Object 0x1C12 */
    {NULL, NULL, 0x1C12, {DEFTYPE_UNSIGNED16, 1 | (OBJCODE_ARR << 8)}, asEntryDesc0x1C12,
     aName0x1C12, &sRxPDOassign, NULL, NULL, 0x0000},
    /* Object 0x1C13 */
    {NULL, NULL, 0x1C13, {DEFTYPE_UNSIGNED16, 1 | (OBJCODE_ARR << 8)}, asEntryDesc0x1C13,
     aName0x1C13, &sTxPDOassign, NULL, NULL, 0x0000},
    /* Object 0x6000 */
    {NULL, NULL, 0x6000, {DEFTYPE_UNSIGNED32, RMCS_STREAM_ENTRY_COUNT | (OBJCODE_ARR << 8)},
     asEntryDesc0x6000, aName0x6000, &sInputStream0x6000, NULL, NULL, 0x0000},
    /* Object 0x7010 */
    {NULL, NULL, 0x7010, {DEFTYPE_UNSIGNED32, RMCS_STREAM_ENTRY_COUNT | (OBJCODE_ARR << 8)},
     asEntryDesc0x7010, aName0x7010, &sOutputStream0x7010, NULL, NULL, 0x0000},
    /* Object 0xF000 */
    {NULL, NULL, 0xF000, {DEFTYPE_RECORD, 2 | (OBJCODE_REC << 8)}, asEntryDesc0xF000, aName0xF000,
     &sModularDeviceProfile0xF000, NULL, NULL, 0x0000},
    {NULL, NULL, 0xFFFF, {0, 0}, NULL, NULL, NULL, NULL}};
#endif /* _OBJD_ */

#undef PROTO

#ifndef _DIGITAL_IO_OBJECTS_H_
#define _DIGITAL_IO_OBJECTS_H_
#endif
