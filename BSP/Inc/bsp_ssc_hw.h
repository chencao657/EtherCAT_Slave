#ifndef __BSP_SSC_HW_H__
#define __BSP_SSC_HW_H__

/*
 * SSC 对象字典和邮箱结构描述的是 EtherCAT 线上的二进制数据格式。
 * 使用 GCC 编译时必须保持字节紧凑布局，否则 UINT16 子索引后的 UINT32
 * 字段会被自动填充，导致 PDO 映射长度读取错误。
 */
#if defined(__GNUC__)
#define SSC_GCC_PACKED __attribute__((packed))
#else
#define SSC_GCC_PACKED
#endif

#ifndef STRUCT_PACKED_START
#define STRUCT_PACKED_START
#endif

#ifndef STRUCT_PACKED_END
#define STRUCT_PACKED_END SSC_GCC_PACKED
#endif

#ifndef MBX_STRUCT_PACKED_START
#define MBX_STRUCT_PACKED_START
#endif

#ifndef MBX_STRUCT_PACKED_END
#define MBX_STRUCT_PACKED_END SSC_GCC_PACKED
#endif

#ifndef OBJ_STRUCT_PACKED_START
#define OBJ_STRUCT_PACKED_START
#endif

#ifndef OBJ_STRUCT_PACKED_END
#define OBJ_STRUCT_PACKED_END SSC_GCC_PACKED
#endif

#ifndef OBJ_DWORD_ALIGN
#define OBJ_DWORD_ALIGN 0
#endif

#ifndef OBJ_WORD_ALIGN
#define OBJ_WORD_ALIGN 0
#endif

/* SSC 协议栈基础类型定义。 */
#include "ecat_def.h"

/* SSC 硬件适配层初始化与释放接口。 */
UINT16 HW_Init(void);
void HW_Release(void);

/* SSC 使用的 1ms 软件计时接口。 */
UINT32 HW_GetTimer(void);
void HW_ClearTimer(void);
void HW_TimerTick1ms(void);

/* 读取 ESC AL Event 事件寄存器。 */
UINT16 HW_GetALEventRegister(void);
UINT16 HW_GetALEventRegister_Isr(void);

/* ESC 中断开关接口，支持嵌套式禁用深度。 */
void HW_EnableEscInterrupt(void);
void HW_DisableEscInterrupt(void);

/* ESC 地址空间普通上下文读写接口。 */
void HW_EscRead(MEM_ADDR *pData, UINT16 address, UINT16 len);
void HW_EscWrite(MEM_ADDR *pData, UINT16 address, UINT16 len);
/* ESC 地址空间中断上下文读写接口。 */
void HW_EscReadIsr(MEM_ADDR *pData, UINT16 address, UINT16 len);
void HW_EscWriteIsr(MEM_ADDR *pData, UINT16 address, UINT16 len);
/* 邮箱内存访问接口，供 SSC 邮箱层调用。 */
void HW_EscReadMbxMem(MEM_ADDR *pData, UINT16 address, UINT16 len);
void HW_EscWriteMbxMem(MEM_ADDR *pData, UINT16 address, UINT16 len);

/* SSC 协议栈期望的中断控制宏。 */
#define DISABLE_ESC_INT() \
    do { HW_DisableEscInterrupt(); } while (0)

#define ENABLE_ESC_INT() \
    do { HW_EnableEscInterrupt(); } while (0)

/* SSC 协议栈期望的 16/32 位 ESC 访问宏。 */
#define HW_EscReadWord(var, address) \
    do { HW_EscRead((MEM_ADDR *)&(var), (address), 2U); } while (0)

#define HW_EscWriteWord(var, address) \
    do { HW_EscWrite((MEM_ADDR *)&(var), (address), 2U); } while (0)

#define HW_EscReadDWord(var, address) \
    do { HW_EscRead((MEM_ADDR *)&(var), (address), 4U); } while (0)

#define HW_EscWriteDWord(var, address) \
    do { HW_EscWrite((MEM_ADDR *)&(var), (address), 4U); } while (0)

/* SSC 协议栈期望的 ISR 版 16/32 位 ESC 访问宏。 */
#define HW_EscReadWordIsr(var, address) \
    do { HW_EscReadIsr((MEM_ADDR *)&(var), (address), 2U); } while (0)

#define HW_EscWriteWordIsr(var, address) \
    do { HW_EscWriteIsr((MEM_ADDR *)&(var), (address), 2U); } while (0)

#define HW_EscReadDWordIsr(var, address) \
    do { HW_EscReadIsr((MEM_ADDR *)&(var), (address), 4U); } while (0)

#define HW_EscWriteDWordIsr(var, address) \
    do { HW_EscWriteIsr((MEM_ADDR *)&(var), (address), 4U); } while (0)

#endif /* __BSP_SSC_HW_H__ */
