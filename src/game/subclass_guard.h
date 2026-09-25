#ifndef SUBCLASS_GUARD_H
#define SUBCLASS_GUARD_H

#include <stdbool.h>

/**
 * Initializes the crash guard for eoc::character_creation::GetAvailableSubClassesForLevelUp.
 * 
 * Prevents EXC_BAD_ACCESS null pointer dereferences when a character or origin
 * has a ClassDescription or SubClass GUID that is not registered in eoc::ClassDescriptions.
 *
 * @param binary_base Base address of the main executable image.
 * @return true if the guard hook was installed successfully, false otherwise.
 */
bool subclass_guard_init(void *binary_base);

#endif /* SUBCLASS_GUARD_H */
