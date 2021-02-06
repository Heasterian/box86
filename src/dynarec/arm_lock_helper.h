#ifndef __ARM_LOCK_HELPER__H__
#define __ARM_LOCK_HELPER__H__
#include <stdint.h>

// LDREXB of ADDR
extern uint8_t arm_lock_read_b(void* addr);
// STREXB of ADDR, return 0 if ok, 1 if not
extern int arm_lock_write_b(void* addr, uint8_t val);

// LDREXH of ADDR
extern uint16_t arm_lock_read_h(void* addr);
// STREXH of ADDR, return 0 if ok, 1 if not
extern int arm_lock_write_h(void* addr, uint16_t val);

// LDREX of ADDR
extern uint32_t arm_lock_read_d(void* addr);
// STREX of ADDR, return 0 if ok, 1 if not
extern int arm_lock_write_d(void* addr, uint32_t val);

// LDREXD of ADDR
extern void arm_lock_read_dd(uint32_t* a, uint32_t* b, void* addr);
// STREX of ADDR, return 0 if ok, 1 if not
extern int arm_lock_write_dd(uint32_t a, uint32_t b, void* addr);

// Atomicaly exchange value at [p] with val, return old p
extern uintptr_t arm_lock_xchg(void* p, uintptr_t val);

// Atomicaly store value to [p] only if [p] is NULL. Return new [p] value (so val or old)
extern void* arm_lock_storeifnull(void*p, void* val);

#endif  //__ARM_LOCK_HELPER__H__