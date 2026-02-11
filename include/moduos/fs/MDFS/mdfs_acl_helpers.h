#ifndef MODUOS_FS_MDFS_ACL_HELPERS_H
#define MODUOS_FS_MDFS_ACL_HELPERS_H

#include "moduos/fs/MDFS/mdfs.h"
#include "moduos/fs/MDFS/mdfs_acl.h"
#include <stdint.h>
#include <stdbool.h>

/**
 * @file mdfs_acl_helpers.h
 * @brief Helper functions to integrate ACL with MDFS inodes
 */

/**
 * @brief Get ACL from inode
 * 
 * @param ino Inode structure
 * @param acl Output ACL structure
 */
void mdfs_inode_get_acl(const mdfs_inode_t *ino, mdfs_acl_t *acl);

/**
 * @brief Set ACL in inode
 * 
 * @param ino Inode structure
 * @param acl ACL to set
 */
void mdfs_inode_set_acl(mdfs_inode_t *ino, const mdfs_acl_t *acl);

/**
 * @brief Check if current process has permission to access inode
 * 
 * @param ino Inode to check
 * @param uid User ID
 * @param gid Primary group ID
 * @param groups Supplementary group IDs
 * @param group_count Number of supplementary groups
 * @param requested Requested permissions
 * @return true if access granted
 */
bool mdfs_inode_check_permission(const mdfs_inode_t *ino,
                                 uint32_t uid,
                                 uint32_t gid,
                                 const uint16_t *groups,
                                 uint8_t group_count,
                                 uint8_t requested);

/**
 * @brief Initialize ACL for new inode
 * 
 * @param ino Inode to initialize
 * @param owner_uid Owner user ID
 * @param owner_gid Owner group ID
 */
void mdfs_inode_init_acl(mdfs_inode_t *ino, uint32_t owner_uid, uint32_t owner_gid);

#endif /* MODUOS_FS_MDFS_ACL_HELPERS_H */
