#ifndef MODUOS_FS_MDFS_ACL_H
#define MODUOS_FS_MDFS_ACL_H

#include <stdint.h>
#include <stdbool.h>

/**
 * @file mdfs_acl.h
 * @brief NTFS-like ACL (Access Control List) implementation for MDFS
 * 
 * This module provides Windows NTFS-style permissions with:
 * - Fine-grained permissions (read, write, execute, delete, ownership)
 * - Allow/Deny ACEs (Access Control Entries)
 * - User and group-based access control
 * - Deny entries override Allow entries
 */

/* Permission flags (5 bits) */
typedef enum {
    MDFS_PERM_READ      = 1 << 0,  /* Read file content or list directory */
    MDFS_PERM_WRITE     = 1 << 1,  /* Modify file content */
    MDFS_PERM_EXECUTE   = 1 << 2,  /* Execute file or traverse directory */
    MDFS_PERM_DELETE    = 1 << 3,  /* Delete file or directory */
    MDFS_PERM_OWNERSHIP = 1 << 4   /* Change owner, modify ACL */
} mdfs_permission_t;

/* ACE (Access Control Entry) type */
typedef enum {
    MDFS_ACE_ALLOW = 0,
    MDFS_ACE_DENY  = 1
} mdfs_ace_type_t;

/* ACE structure (32-bit compact format)
 * Bit layout: [Reserved:9][IsGroup:1][Type:1][Perms:5][ID:16]
 */
typedef uint32_t mdfs_ace_t;

#define MDFS_ACE_MAKE(id, is_group, type, perms) \
    (((uint32_t)(id) & 0xFFFF) | \
     (((uint32_t)(perms) & 0x1F) << 16) | \
     (((uint32_t)(type) & 0x1) << 21) | \
     (((uint32_t)(is_group) & 0x1) << 22))

#define MDFS_ACE_GET_ID(ace)          ((uint16_t)((ace) & 0xFFFF))
#define MDFS_ACE_GET_PERMISSIONS(ace) ((uint8_t)(((ace) >> 16) & 0x1F))
#define MDFS_ACE_GET_TYPE(ace)        ((mdfs_ace_type_t)(((ace) >> 21) & 0x1))
#define MDFS_ACE_GET_IS_GROUP(ace)    ((bool)(((ace) >> 22) & 0x1))

/* Maximum ACEs per file/directory */
#define MDFS_MAX_ACES 16

/* ACL structure (stored in inode extended attributes or separate block) */
typedef struct __attribute__((packed)) {
    uint8_t ace_count;        /* Number of ACEs (0-16) */
    uint8_t _reserved[3];     /* Padding for alignment */
    mdfs_ace_t aces[MDFS_MAX_ACES]; /* ACE array (64 bytes) */
} mdfs_acl_t;

/* Ensure ACL fits in available inode padding */
_Static_assert(sizeof(mdfs_acl_t) == 68, "mdfs_acl_t must be 68 bytes");

/**
 * @brief Check if user has requested permissions on a file/directory
 * 
 * @param acl ACL to check
 * @param uid User ID
 * @param gids Array of group IDs user belongs to
 * @param gid_count Number of groups
 * @param requested Permissions to check (bitwise OR of mdfs_permission_t)
 * @return true if access granted, false if denied
 * 
 * Logic: Deny entries always override Allow entries.
 * If no ACL exists, fall back to traditional Unix permissions.
 */
bool mdfs_acl_check_permission(const mdfs_acl_t *acl, 
                               uint16_t uid, 
                               const uint16_t *gids, 
                               int gid_count,
                               uint8_t requested);

/**
 * @brief Initialize an empty ACL
 */
void mdfs_acl_init(mdfs_acl_t *acl);

/**
 * @brief Add an ACE to an ACL
 * 
 * @return 0 on success, -1 if ACL is full
 */
int mdfs_acl_add_ace(mdfs_acl_t *acl, uint16_t id, bool is_group, 
                     mdfs_ace_type_t type, uint8_t perms);

/**
 * @brief Remove an ACE from an ACL by index
 * 
 * @return 0 on success, -1 if index invalid
 */
int mdfs_acl_remove_ace(mdfs_acl_t *acl, int index);

/**
 * @brief Clear all ACEs from an ACL
 */
void mdfs_acl_clear(mdfs_acl_t *acl);

/**
 * @brief Create a default ACL for a new file/directory
 * 
 * Creates:
 * - Allow owner all permissions
 * - Allow owner's primary group read/execute
 * 
 * @param acl ACL to initialize
 * @param owner_uid Owner user ID
 * @param owner_gid Owner group ID
 */
void mdfs_acl_create_default(mdfs_acl_t *acl, uint16_t owner_uid, uint16_t owner_gid);

#endif /* MODUOS_FS_MDFS_ACL_H */
