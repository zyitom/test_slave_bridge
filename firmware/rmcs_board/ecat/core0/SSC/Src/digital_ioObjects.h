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
 * Two layouts are selected at compile time:
 *
 *  - Stock (#else): one 48-byte stream chunk per direction (see
 *    common/pd_stream.hpp), described to CoE as arrays of 12 UNSIGNED32
 *    because a single PDO-mapping entry cannot exceed 255 bits (8-bit length
 *    field, ETG.1000.6). 0x1A00 maps 0x6000:01..:0C (inputs, slave -> master),
 *    0x1600 maps 0x7010:01..:0C (outputs, master -> slave).
 *
 *  - Hybrid (RMCS_ECAT_HYBRID_PD): 352 bytes per direction = 28 x 12-byte
 *    cyclic CAN slots + a 16-byte pd_stream chunk (see
 *    librmcs/ecat/hybrid_pd.hpp). Each direction maps two objects: an 84 x
 *    UNSIGNED32 fixed array then a 4 x UNSIGNED32 stream array. 0x1600 maps
 *    0x7000:01..:54 (outputs) + 0x7010:01..:04 (stream); 0x1A00 maps
 *    0x6000:01..:54 (inputs) + 0x6010:01..:04 (stream).
 *
 * Sizes must agree with RMCS_PD_CHUNK_SIZE (rmcs_pd.h) and MAX_PD_*_SIZE
 * (core0/CMakeLists.txt); ecat_appl.c static-asserts the arithmetic.
 *
 * The object variables below are CoE/SDO-visible placeholders only: the
 * cyclic process-data bytes are moved directly between the ESC process data
 * image and the cross-core rings by APPL_InputMapping()/APPL_OutputMapping(),
 * never through these variables.
 */

/* Marker checked by core0/CMakeLists.txt to reject a stock generated file. */
#define RMCS_STREAM_OBJECTS 1

#if defined(_DIGITAL_IO_) && (_DIGITAL_IO_ == 1)
# define PROTO
#else
# define PROTO extern
#endif

/* Mapping entry (index << 16) | (subindex << 8) | bit length. */
#define RMCS_MAP_ENTRY(index, subindex) (((UINT32)(index) << 16) | ((subindex) << 8) | 0x20U)

#if defined(RMCS_ECAT_HYBRID_PD) && RMCS_ECAT_HYBRID_PD

/*======================================================================
 *  HYBRID FIXED-PDO LAYOUT
 *====================================================================*/

# define RMCS_STREAM_ENTRY_COUNT  88 /* 88 x UNSIGNED32 = 352 bytes per direction */
# define RMCS_HYBRID_MBX_COUNT    84 /* 84 x UNSIGNED32 = 336-byte fixed region */
# define RMCS_HYBRID_STREAM_COUNT 4  /* 4 x UNSIGNED32 = 16-byte stream chunk */

# define RMCS_DESC_U32 {DEFTYPE_UNSIGNED32, 0x20, ACCESS_READ}
# define RMCS_DESC_2   RMCS_DESC_U32, RMCS_DESC_U32
# define RMCS_DESC_4   RMCS_DESC_2, RMCS_DESC_2
# define RMCS_DESC_8   RMCS_DESC_4, RMCS_DESC_4
# define RMCS_DESC_16  RMCS_DESC_8, RMCS_DESC_8
# define RMCS_DESC_32  RMCS_DESC_16, RMCS_DESC_16
# define RMCS_DESC_64  RMCS_DESC_32, RMCS_DESC_32

# define RMCS_MAP_4(index, first)                                      \
     RMCS_MAP_ENTRY(index, first), RMCS_MAP_ENTRY(index, (first) + 1), \
         RMCS_MAP_ENTRY(index, (first) + 2), RMCS_MAP_ENTRY(index, (first) + 3)
# define RMCS_MAP_16(index, first)                                                             \
     RMCS_MAP_4(index, first), RMCS_MAP_4(index, (first) + 4), RMCS_MAP_4(index, (first) + 8), \
         RMCS_MAP_4(index, (first) + 12)
# define RMCS_MAP_64(index, first)                                \
     RMCS_MAP_16(index, first), RMCS_MAP_16(index, (first) + 16), \
         RMCS_MAP_16(index, (first) + 32), RMCS_MAP_16(index, (first) + 48)

/******************************************************************************
 *                    Object 0x1600 : Output process data mapping
 ******************************************************************************/
# ifdef _OBJD_
OBJCONST TSDOINFOENTRYDESC OBJMEM asEntryDesc0x1600[] = {
    {DEFTYPE_UNSIGNED8, 0x8, ACCESS_READ},
    RMCS_DESC_64, RMCS_DESC_16, RMCS_DESC_8
};

OBJCONST UCHAR OBJMEM aName0x1600[] = "Output process data mapping\000\377";
# endif /* _OBJD_ */

# ifndef _DIGITAL_IO_OBJECTS_H_
typedef struct OBJ_STRUCT_PACKED_START {
    UINT16 u16SubIndex0;
    UINT32 aEntries[RMCS_STREAM_ENTRY_COUNT];
} OBJ_STRUCT_PACKED_END TOBJ1600;
# endif /* _DIGITAL_IO_OBJECTS_H_ */

PROTO TOBJ1600 sOutputMapping0x1600
# if defined(_DIGITAL_IO_) && (_DIGITAL_IO_ == 1)
    =
        {
            RMCS_STREAM_ENTRY_COUNT,
            {RMCS_MAP_64(0x7000, 1), RMCS_MAP_16(0x7000, 65), RMCS_MAP_4(0x7000, 81),
              RMCS_MAP_4(0x7010, 1)}
}
# endif
;

/******************************************************************************
 *                    Object 0x1A00 : Input process data mapping
 ******************************************************************************/
# ifdef _OBJD_
OBJCONST TSDOINFOENTRYDESC OBJMEM asEntryDesc0x1A00[] = {
    {DEFTYPE_UNSIGNED8, 0x8, ACCESS_READ},
    RMCS_DESC_64, RMCS_DESC_16, RMCS_DESC_8
};

OBJCONST UCHAR OBJMEM aName0x1A00[] = "Input process data mapping\000\377";
# endif /* _OBJD_ */

# ifndef _DIGITAL_IO_OBJECTS_H_
typedef struct OBJ_STRUCT_PACKED_START {
    UINT16 u16SubIndex0;
    UINT32 aEntries[RMCS_STREAM_ENTRY_COUNT];
} OBJ_STRUCT_PACKED_END TOBJ1A00;
# endif /* _DIGITAL_IO_OBJECTS_H_ */

PROTO TOBJ1A00 sInputMapping0x1A00
# if defined(_DIGITAL_IO_) && (_DIGITAL_IO_ == 1)
    =
        {
            RMCS_STREAM_ENTRY_COUNT,
            {RMCS_MAP_64(0x6000, 1), RMCS_MAP_16(0x6000, 65), RMCS_MAP_4(0x6000, 81),
              RMCS_MAP_4(0x6010, 1)}
}
# endif
;

/******************************************************************************
 *                    Object 0x1C12 : SyncManager 2 assignment
 ******************************************************************************/
# ifdef _OBJD_
OBJCONST TSDOINFOENTRYDESC OBJMEM asEntryDesc0x1C12[] = {
    { DEFTYPE_UNSIGNED8,  0x8, ACCESS_READ},
    {DEFTYPE_UNSIGNED16, 0x10, ACCESS_READ}
};

OBJCONST UCHAR OBJMEM aName0x1C12[] = "SyncManager 2 assignment\000\377";
# endif /* _OBJD_ */

# ifndef _DIGITAL_IO_OBJECTS_H_
typedef struct OBJ_STRUCT_PACKED_START {
    UINT16 u16SubIndex0;
    UINT16 aEntries[1];
} OBJ_STRUCT_PACKED_END TOBJ1C12;
# endif /* _DIGITAL_IO_OBJECTS_H_ */

PROTO TOBJ1C12 sRxPDOassign
# if defined(_DIGITAL_IO_) && (_DIGITAL_IO_ == 1)
    = {1, {0x1600}}
# endif
;

/******************************************************************************
 *                    Object 0x1C13 : SyncManager 3 assignment
 ******************************************************************************/
# ifdef _OBJD_
OBJCONST TSDOINFOENTRYDESC OBJMEM asEntryDesc0x1C13[] = {
    { DEFTYPE_UNSIGNED8,  0x8, ACCESS_READ},
    {DEFTYPE_UNSIGNED16, 0x10, ACCESS_READ}
};

OBJCONST UCHAR OBJMEM aName0x1C13[] = "SyncManager 3 assignment\000\377";
# endif /* _OBJD_ */

# ifndef _DIGITAL_IO_OBJECTS_H_
typedef struct OBJ_STRUCT_PACKED_START {
    UINT16 u16SubIndex0;
    UINT16 aEntries[1];
} OBJ_STRUCT_PACKED_END TOBJ1C13;
# endif /* _DIGITAL_IO_OBJECTS_H_ */

PROTO TOBJ1C13 sTxPDOassign
# if defined(_DIGITAL_IO_) && (_DIGITAL_IO_ == 1)
    = {1, {0x1A00}}
# endif
;

/******************************************************************************
 *                    Object 0x6000 : InputMailboxes (slave -> master)
 ******************************************************************************/
# ifdef _OBJD_
OBJCONST TSDOINFOENTRYDESC OBJMEM asEntryDesc0x6000[] = {
    { DEFTYPE_UNSIGNED8,  0x8,                          ACCESS_READ},
    {DEFTYPE_UNSIGNED32, 0x20, ACCESS_READ | OBJACCESS_TXPDOMAPPING}
};

OBJCONST UCHAR OBJMEM aName0x6000[] = "InputMailboxes\000\377";
# endif /* _OBJD_ */

# ifndef _DIGITAL_IO_OBJECTS_H_
typedef struct OBJ_STRUCT_PACKED_START {
    UINT16 u16SubIndex0;
    UINT32 aData[RMCS_HYBRID_MBX_COUNT];
} OBJ_STRUCT_PACKED_END TOBJ6000;
# endif /* _DIGITAL_IO_OBJECTS_H_ */

PROTO TOBJ6000 sInputMailboxes0x6000
# if defined(_DIGITAL_IO_) && (_DIGITAL_IO_ == 1)
    = {RMCS_HYBRID_MBX_COUNT, {0}}
# endif
;

/******************************************************************************
 *                    Object 0x6010 : InputStream (slave -> master)
 ******************************************************************************/
# ifdef _OBJD_
OBJCONST TSDOINFOENTRYDESC OBJMEM asEntryDesc0x6010[] = {
    { DEFTYPE_UNSIGNED8,  0x8,                          ACCESS_READ},
    {DEFTYPE_UNSIGNED32, 0x20, ACCESS_READ | OBJACCESS_TXPDOMAPPING}
};

OBJCONST UCHAR OBJMEM aName0x6010[] = "InputStream\000\377";
# endif /* _OBJD_ */

# ifndef _DIGITAL_IO_OBJECTS_H_
typedef struct OBJ_STRUCT_PACKED_START {
    UINT16 u16SubIndex0;
    UINT32 aData[RMCS_HYBRID_STREAM_COUNT];
} OBJ_STRUCT_PACKED_END TOBJ6010;
# endif /* _DIGITAL_IO_OBJECTS_H_ */

PROTO TOBJ6010 sInputStream0x6010
# if defined(_DIGITAL_IO_) && (_DIGITAL_IO_ == 1)
    = {RMCS_HYBRID_STREAM_COUNT, {0}}
# endif
;

/******************************************************************************
 *                    Object 0x7000 : OutputMailboxes (master -> slave)
 ******************************************************************************/
# ifdef _OBJD_
OBJCONST TSDOINFOENTRYDESC OBJMEM asEntryDesc0x7000[] = {
    { DEFTYPE_UNSIGNED8,  0x8,                               ACCESS_READ},
    {DEFTYPE_UNSIGNED32, 0x20, ACCESS_READWRITE | OBJACCESS_RXPDOMAPPING}
};

OBJCONST UCHAR OBJMEM aName0x7000[] = "OutputMailboxes\000\377";
# endif /* _OBJD_ */

# ifndef _DIGITAL_IO_OBJECTS_H_
typedef struct OBJ_STRUCT_PACKED_START {
    UINT16 u16SubIndex0;
    UINT32 aData[RMCS_HYBRID_MBX_COUNT];
} OBJ_STRUCT_PACKED_END TOBJ7000;
# endif /* _DIGITAL_IO_OBJECTS_H_ */

PROTO TOBJ7000 sOutputMailboxes0x7000
# if defined(_DIGITAL_IO_) && (_DIGITAL_IO_ == 1)
    = {RMCS_HYBRID_MBX_COUNT, {0}}
# endif
;

/******************************************************************************
 *                    Object 0x7010 : OutputStream (master -> slave)
 ******************************************************************************/
# ifdef _OBJD_
OBJCONST TSDOINFOENTRYDESC OBJMEM asEntryDesc0x7010[] = {
    { DEFTYPE_UNSIGNED8,  0x8,                               ACCESS_READ},
    {DEFTYPE_UNSIGNED32, 0x20, ACCESS_READWRITE | OBJACCESS_RXPDOMAPPING}
};

OBJCONST UCHAR OBJMEM aName0x7010[] = "OutputStream\000\377";
# endif /* _OBJD_ */

# ifndef _DIGITAL_IO_OBJECTS_H_
typedef struct OBJ_STRUCT_PACKED_START {
    UINT16 u16SubIndex0;
    UINT32 aData[RMCS_HYBRID_STREAM_COUNT];
} OBJ_STRUCT_PACKED_END TOBJ7010;
# endif /* _DIGITAL_IO_OBJECTS_H_ */

PROTO TOBJ7010 sOutputStream0x7010
# if defined(_DIGITAL_IO_) && (_DIGITAL_IO_ == 1)
    = {RMCS_HYBRID_STREAM_COUNT, {0}}
# endif
;

/******************************************************************************
 *                    Object 0xF000 : Modular Device Profile
 ******************************************************************************/
# ifdef _OBJD_
OBJCONST TSDOINFOENTRYDESC OBJMEM asEntryDesc0xF000[] = {
    { DEFTYPE_UNSIGNED8,  0x8, ACCESS_READ},
    {DEFTYPE_UNSIGNED16, 0x10, ACCESS_READ},
    {DEFTYPE_UNSIGNED16, 0x10, ACCESS_READ}
};

OBJCONST UCHAR OBJMEM aName0xF000[] = "Modular Device Profile\000"
                                      "Index distance \000"
                                      "Maximum number of modules \000\377";
# endif /* _OBJD_ */

# ifndef _DIGITAL_IO_OBJECTS_H_
typedef struct OBJ_STRUCT_PACKED_START {
    UINT16 u16SubIndex0;
    UINT16 IndexDistance;
    UINT16 MaximumNumberOfModules;
} OBJ_STRUCT_PACKED_END TOBJF000;
# endif /* _DIGITAL_IO_OBJECTS_H_ */

PROTO TOBJF000 sModularDeviceProfile0xF000
# if defined(_DIGITAL_IO_) && (_DIGITAL_IO_ == 1)
    = {2, 0x0010, 0}
# endif
;

# ifdef _OBJD_
TOBJECT OBJMEM ApplicationObjDic[] = {
    /* Object 0x1600 */
    {NULL,
     NULL, 0x1600,
     {DEFTYPE_PDOMAPPING, RMCS_STREAM_ENTRY_COUNT | (OBJCODE_REC << 8)},
     asEntryDesc0x1600, aName0x1600,
     &sOutputMapping0x1600,
     NULL, NULL,
     0x0000},
    /* Object 0x1A00 */
    {NULL,
     NULL, 0x1A00,
     {DEFTYPE_PDOMAPPING, RMCS_STREAM_ENTRY_COUNT | (OBJCODE_REC << 8)},
     asEntryDesc0x1A00, aName0x1A00,
     &sInputMapping0x1A00,
     NULL, NULL,
     0x0000},
    /* Object 0x1C12 */
    {NULL,
     NULL, 0x1C12,
     {DEFTYPE_UNSIGNED16, 1 | (OBJCODE_ARR << 8)},
     asEntryDesc0x1C12, aName0x1C12,
     &sRxPDOassign,
     NULL, NULL,
     0x0000},
    /* Object 0x1C13 */
    {NULL,
     NULL, 0x1C13,
     {DEFTYPE_UNSIGNED16, 1 | (OBJCODE_ARR << 8)},
     asEntryDesc0x1C13, aName0x1C13,
     &sTxPDOassign,
     NULL, NULL,
     0x0000},
    /* Object 0x6000 */
    {NULL,
     NULL, 0x6000,
     {DEFTYPE_UNSIGNED32, RMCS_HYBRID_MBX_COUNT | (OBJCODE_ARR << 8)},
     asEntryDesc0x6000, aName0x6000,
     &sInputMailboxes0x6000,
     NULL, NULL,
     0x0000},
    /* Object 0x6010 */
    {NULL,
     NULL, 0x6010,
     {DEFTYPE_UNSIGNED32, RMCS_HYBRID_STREAM_COUNT | (OBJCODE_ARR << 8)},
     asEntryDesc0x6010, aName0x6010,
     &sInputStream0x6010,
     NULL, NULL,
     0x0000},
    /* Object 0x7000 */
    {NULL,
     NULL, 0x7000,
     {DEFTYPE_UNSIGNED32, RMCS_HYBRID_MBX_COUNT | (OBJCODE_ARR << 8)},
     asEntryDesc0x7000, aName0x7000,
     &sOutputMailboxes0x7000,
     NULL, NULL,
     0x0000},
    /* Object 0x7010 */
    {NULL,
     NULL, 0x7010,
     {DEFTYPE_UNSIGNED32, RMCS_HYBRID_STREAM_COUNT | (OBJCODE_ARR << 8)},
     asEntryDesc0x7010, aName0x7010,
     &sOutputStream0x7010,
     NULL, NULL,
     0x0000},
    /* Object 0xF000 */
    {NULL,
     NULL, 0xF000,
     {DEFTYPE_RECORD, 2 | (OBJCODE_REC << 8)},
     asEntryDesc0xF000, aName0xF000,
     &sModularDeviceProfile0xF000,
     NULL, NULL,
     0x0000},
    {NULL, NULL, 0xFFFF, {0, 0}, NULL, NULL, NULL, NULL}
};
# endif /* _OBJD_ */

# undef RMCS_MAP_64
# undef RMCS_MAP_16
# undef RMCS_MAP_4
# undef RMCS_DESC_64
# undef RMCS_DESC_32
# undef RMCS_DESC_16
# undef RMCS_DESC_8
# undef RMCS_DESC_4
# undef RMCS_DESC_2
# undef RMCS_DESC_U32

#else                               /* !RMCS_ECAT_HYBRID_PD : stock 48-byte stream layout */

# define RMCS_STREAM_ENTRY_COUNT 12 /* 12 x UNSIGNED32 = 48 bytes */

/******************************************************************************
 *                    Object 0x1600 : OutputStream process data mapping
 ******************************************************************************/
# ifdef _OBJD_
OBJCONST TSDOINFOENTRYDESC OBJMEM asEntryDesc0x1600[] = {
    { DEFTYPE_UNSIGNED8,  0x8, ACCESS_READ},
    {DEFTYPE_UNSIGNED32, 0x20, ACCESS_READ},
    {DEFTYPE_UNSIGNED32, 0x20, ACCESS_READ},
    {DEFTYPE_UNSIGNED32, 0x20, ACCESS_READ},
    {DEFTYPE_UNSIGNED32, 0x20, ACCESS_READ},
    {DEFTYPE_UNSIGNED32, 0x20, ACCESS_READ},
    {DEFTYPE_UNSIGNED32, 0x20, ACCESS_READ},
    {DEFTYPE_UNSIGNED32, 0x20, ACCESS_READ},
    {DEFTYPE_UNSIGNED32, 0x20, ACCESS_READ},
    {DEFTYPE_UNSIGNED32, 0x20, ACCESS_READ},
    {DEFTYPE_UNSIGNED32, 0x20, ACCESS_READ},
    {DEFTYPE_UNSIGNED32, 0x20, ACCESS_READ},
    {DEFTYPE_UNSIGNED32, 0x20, ACCESS_READ}
};

OBJCONST UCHAR OBJMEM aName0x1600[] = "OutputStream process data mapping\000\377";
# endif                             /* _OBJD_ */

# ifndef _DIGITAL_IO_OBJECTS_H_
typedef struct OBJ_STRUCT_PACKED_START {
    UINT16 u16SubIndex0;
    UINT32 aEntries[RMCS_STREAM_ENTRY_COUNT];
} OBJ_STRUCT_PACKED_END TOBJ1600;
# endif /* _DIGITAL_IO_OBJECTS_H_ */

PROTO TOBJ1600 sOutputStreamMapping0x1600
# if defined(_DIGITAL_IO_) && (_DIGITAL_IO_ == 1)
    =
        {
            RMCS_STREAM_ENTRY_COUNT,
            {RMCS_MAP_ENTRY(0x7010, 1), RMCS_MAP_ENTRY(0x7010, 2), RMCS_MAP_ENTRY(0x7010, 3),
              RMCS_MAP_ENTRY(0x7010, 4), RMCS_MAP_ENTRY(0x7010, 5), RMCS_MAP_ENTRY(0x7010, 6),
              RMCS_MAP_ENTRY(0x7010, 7), RMCS_MAP_ENTRY(0x7010, 8), RMCS_MAP_ENTRY(0x7010, 9),
              RMCS_MAP_ENTRY(0x7010, 10), RMCS_MAP_ENTRY(0x7010, 11), RMCS_MAP_ENTRY(0x7010, 12)}
}
# endif
;

/******************************************************************************
 *                    Object 0x1A00 : InputStream process data mapping
 ******************************************************************************/
# ifdef _OBJD_
OBJCONST TSDOINFOENTRYDESC OBJMEM asEntryDesc0x1A00[] = {
    { DEFTYPE_UNSIGNED8,  0x8, ACCESS_READ},
    {DEFTYPE_UNSIGNED32, 0x20, ACCESS_READ},
    {DEFTYPE_UNSIGNED32, 0x20, ACCESS_READ},
    {DEFTYPE_UNSIGNED32, 0x20, ACCESS_READ},
    {DEFTYPE_UNSIGNED32, 0x20, ACCESS_READ},
    {DEFTYPE_UNSIGNED32, 0x20, ACCESS_READ},
    {DEFTYPE_UNSIGNED32, 0x20, ACCESS_READ},
    {DEFTYPE_UNSIGNED32, 0x20, ACCESS_READ},
    {DEFTYPE_UNSIGNED32, 0x20, ACCESS_READ},
    {DEFTYPE_UNSIGNED32, 0x20, ACCESS_READ},
    {DEFTYPE_UNSIGNED32, 0x20, ACCESS_READ},
    {DEFTYPE_UNSIGNED32, 0x20, ACCESS_READ},
    {DEFTYPE_UNSIGNED32, 0x20, ACCESS_READ}
};

OBJCONST UCHAR OBJMEM aName0x1A00[] = "InputStream process data mapping\000\377";
# endif /* _OBJD_ */

# ifndef _DIGITAL_IO_OBJECTS_H_
typedef struct OBJ_STRUCT_PACKED_START {
    UINT16 u16SubIndex0;
    UINT32 aEntries[RMCS_STREAM_ENTRY_COUNT];
} OBJ_STRUCT_PACKED_END TOBJ1A00;
# endif /* _DIGITAL_IO_OBJECTS_H_ */

PROTO TOBJ1A00 sInputStreamMapping0x1A00
# if defined(_DIGITAL_IO_) && (_DIGITAL_IO_ == 1)
    =
        {
            RMCS_STREAM_ENTRY_COUNT,
            {RMCS_MAP_ENTRY(0x6000, 1), RMCS_MAP_ENTRY(0x6000, 2), RMCS_MAP_ENTRY(0x6000, 3),
              RMCS_MAP_ENTRY(0x6000, 4), RMCS_MAP_ENTRY(0x6000, 5), RMCS_MAP_ENTRY(0x6000, 6),
              RMCS_MAP_ENTRY(0x6000, 7), RMCS_MAP_ENTRY(0x6000, 8), RMCS_MAP_ENTRY(0x6000, 9),
              RMCS_MAP_ENTRY(0x6000, 10), RMCS_MAP_ENTRY(0x6000, 11), RMCS_MAP_ENTRY(0x6000, 12)}
}
# endif
;

/******************************************************************************
 *                    Object 0x1C12 : SyncManager 2 assignment
 ******************************************************************************/
# ifdef _OBJD_
OBJCONST TSDOINFOENTRYDESC OBJMEM asEntryDesc0x1C12[] = {
    { DEFTYPE_UNSIGNED8,  0x8, ACCESS_READ},
    {DEFTYPE_UNSIGNED16, 0x10, ACCESS_READ}
};

OBJCONST UCHAR OBJMEM aName0x1C12[] = "SyncManager 2 assignment\000\377";
# endif /* _OBJD_ */

# ifndef _DIGITAL_IO_OBJECTS_H_
typedef struct OBJ_STRUCT_PACKED_START {
    UINT16 u16SubIndex0;
    UINT16 aEntries[1];
} OBJ_STRUCT_PACKED_END TOBJ1C12;
# endif /* _DIGITAL_IO_OBJECTS_H_ */

PROTO TOBJ1C12 sRxPDOassign
# if defined(_DIGITAL_IO_) && (_DIGITAL_IO_ == 1)
    = {1, {0x1600}}
# endif
;

/******************************************************************************
 *                    Object 0x1C13 : SyncManager 3 assignment
 ******************************************************************************/
# ifdef _OBJD_
OBJCONST TSDOINFOENTRYDESC OBJMEM asEntryDesc0x1C13[] = {
    { DEFTYPE_UNSIGNED8,  0x8, ACCESS_READ},
    {DEFTYPE_UNSIGNED16, 0x10, ACCESS_READ}
};

OBJCONST UCHAR OBJMEM aName0x1C13[] = "SyncManager 3 assignment\000\377";
# endif /* _OBJD_ */

# ifndef _DIGITAL_IO_OBJECTS_H_
typedef struct OBJ_STRUCT_PACKED_START {
    UINT16 u16SubIndex0;
    UINT16 aEntries[1];
} OBJ_STRUCT_PACKED_END TOBJ1C13;
# endif /* _DIGITAL_IO_OBJECTS_H_ */

PROTO TOBJ1C13 sTxPDOassign
# if defined(_DIGITAL_IO_) && (_DIGITAL_IO_ == 1)
    = {1, {0x1A00}}
# endif
;

/******************************************************************************
 *                    Object 0x6000 : InputStream (slave -> master)
 ******************************************************************************/
# ifdef _OBJD_
/* Array object: entry description [0] is subindex 0, [1] is shared by all
 * data subindexes. */
OBJCONST TSDOINFOENTRYDESC OBJMEM asEntryDesc0x6000[] = {
    { DEFTYPE_UNSIGNED8,  0x8,                          ACCESS_READ},
    {DEFTYPE_UNSIGNED32, 0x20, ACCESS_READ | OBJACCESS_TXPDOMAPPING}
};

OBJCONST UCHAR OBJMEM aName0x6000[] = "InputStream\000\377";
# endif /* _OBJD_ */

# ifndef _DIGITAL_IO_OBJECTS_H_
typedef struct OBJ_STRUCT_PACKED_START {
    UINT16 u16SubIndex0;
    UINT32 aData[RMCS_STREAM_ENTRY_COUNT];
} OBJ_STRUCT_PACKED_END TOBJ6000;
# endif /* _DIGITAL_IO_OBJECTS_H_ */

PROTO TOBJ6000 sInputStream0x6000
# if defined(_DIGITAL_IO_) && (_DIGITAL_IO_ == 1)
    = {RMCS_STREAM_ENTRY_COUNT, {0}}
# endif
;

/******************************************************************************
 *                    Object 0x7010 : OutputStream (master -> slave)
 ******************************************************************************/
# ifdef _OBJD_
OBJCONST TSDOINFOENTRYDESC OBJMEM asEntryDesc0x7010[] = {
    { DEFTYPE_UNSIGNED8,  0x8,                               ACCESS_READ},
    {DEFTYPE_UNSIGNED32, 0x20, ACCESS_READWRITE | OBJACCESS_RXPDOMAPPING}
};

OBJCONST UCHAR OBJMEM aName0x7010[] = "OutputStream\000\377";
# endif /* _OBJD_ */

# ifndef _DIGITAL_IO_OBJECTS_H_
typedef struct OBJ_STRUCT_PACKED_START {
    UINT16 u16SubIndex0;
    UINT32 aData[RMCS_STREAM_ENTRY_COUNT];
} OBJ_STRUCT_PACKED_END TOBJ7010;
# endif /* _DIGITAL_IO_OBJECTS_H_ */

PROTO TOBJ7010 sOutputStream0x7010
# if defined(_DIGITAL_IO_) && (_DIGITAL_IO_ == 1)
    = {RMCS_STREAM_ENTRY_COUNT, {0}}
# endif
;

/******************************************************************************
 *                    Object 0xF000 : Modular Device Profile
 ******************************************************************************/
# ifdef _OBJD_
OBJCONST TSDOINFOENTRYDESC OBJMEM asEntryDesc0xF000[] = {
    { DEFTYPE_UNSIGNED8,  0x8, ACCESS_READ},
    {DEFTYPE_UNSIGNED16, 0x10, ACCESS_READ},
    {DEFTYPE_UNSIGNED16, 0x10, ACCESS_READ}
};

OBJCONST UCHAR OBJMEM aName0xF000[] = "Modular Device Profile\000"
                                      "Index distance \000"
                                      "Maximum number of modules \000\377";
# endif /* _OBJD_ */

# ifndef _DIGITAL_IO_OBJECTS_H_
typedef struct OBJ_STRUCT_PACKED_START {
    UINT16 u16SubIndex0;
    UINT16 IndexDistance;
    UINT16 MaximumNumberOfModules;
} OBJ_STRUCT_PACKED_END TOBJF000;
# endif /* _DIGITAL_IO_OBJECTS_H_ */

PROTO TOBJF000 sModularDeviceProfile0xF000
# if defined(_DIGITAL_IO_) && (_DIGITAL_IO_ == 1)
    = {2, 0x0010, 0}
# endif
;

# ifdef _OBJD_
TOBJECT OBJMEM ApplicationObjDic[] = {
    /* Object 0x1600 */
    {NULL,
     NULL, 0x1600,
     {DEFTYPE_PDOMAPPING, RMCS_STREAM_ENTRY_COUNT | (OBJCODE_REC << 8)},
     asEntryDesc0x1600, aName0x1600,
     &sOutputStreamMapping0x1600,
     NULL, NULL,
     0x0000},
    /* Object 0x1A00 */
    {NULL,
     NULL, 0x1A00,
     {DEFTYPE_PDOMAPPING, RMCS_STREAM_ENTRY_COUNT | (OBJCODE_REC << 8)},
     asEntryDesc0x1A00, aName0x1A00,
     &sInputStreamMapping0x1A00,
     NULL, NULL,
     0x0000},
    /* Object 0x1C12 */
    {NULL,
     NULL, 0x1C12,
     {DEFTYPE_UNSIGNED16, 1 | (OBJCODE_ARR << 8)},
     asEntryDesc0x1C12, aName0x1C12,
     &sRxPDOassign,
     NULL, NULL,
     0x0000},
    /* Object 0x1C13 */
    {NULL,
     NULL, 0x1C13,
     {DEFTYPE_UNSIGNED16, 1 | (OBJCODE_ARR << 8)},
     asEntryDesc0x1C13, aName0x1C13,
     &sTxPDOassign,
     NULL, NULL,
     0x0000},
    /* Object 0x6000 */
    {NULL,
     NULL, 0x6000,
     {DEFTYPE_UNSIGNED32, RMCS_STREAM_ENTRY_COUNT | (OBJCODE_ARR << 8)},
     asEntryDesc0x6000, aName0x6000,
     &sInputStream0x6000,
     NULL, NULL,
     0x0000},
    /* Object 0x7010 */
    {NULL,
     NULL, 0x7010,
     {DEFTYPE_UNSIGNED32, RMCS_STREAM_ENTRY_COUNT | (OBJCODE_ARR << 8)},
     asEntryDesc0x7010, aName0x7010,
     &sOutputStream0x7010,
     NULL, NULL,
     0x0000},
    /* Object 0xF000 */
    {NULL,
     NULL, 0xF000,
     {DEFTYPE_RECORD, 2 | (OBJCODE_REC << 8)},
     asEntryDesc0xF000, aName0xF000,
     &sModularDeviceProfile0xF000,
     NULL, NULL,
     0x0000},
    {NULL, NULL, 0xFFFF, {0, 0}, NULL, NULL, NULL, NULL}
};
# endif /* _OBJD_ */

#endif  /* RMCS_ECAT_HYBRID_PD */

#undef PROTO

#ifndef _DIGITAL_IO_OBJECTS_H_
# define _DIGITAL_IO_OBJECTS_H_
#endif
