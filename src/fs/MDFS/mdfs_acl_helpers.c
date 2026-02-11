#include "moduos/fs/MDFS/mdfs_acl_helpers.h"
#include "moduos/kernel/memory/string.h"
#include "moduos/kernel/user_identity.h"

void mdfs_inode_get_acl(const mdfs_inode_t *ino, mdfs_acl_t *acl) {
    if (!ino || !acl) return;

    acl->ace_count = ino->acl_count;
    acl->_reserved[0] = ino->acl_reserved[0];
    acl->_reserved[1] = ino->acl_reserved[1];
    acl->_reserved[2] = ino->acl_reserved[2];
    
    for (int i = 0; i < MDFS_MAX_ACES; i++) {
        acl->aces[i] = ino->acl_aces[i];
    }
}

void mdfs_inode_set_acl(mdfs_inode_t *ino, const mdfs_acl_t *acl) {
    if (!ino || !acl) return;

    ino->acl_count = acl->ace_count;
    ino->acl_reserved[0] = acl->_reserved[0];
    ino->acl_reserved[1] = acl->_reserved[1];
    ino->acl_reserved[2] = acl->_reserved[2];
    
    for (int i = 0; i < MDFS_MAX_ACES; i++) {
        ino->acl_aces[i] = acl->aces[i];
    }
}

bool mdfs_inode_check_permission(const mdfs_inode_t *ino,
                                 uint32_t uid,
                                 uint32_t gid,
                                 const uint16_t *groups,
                                 uint8_t group_count,
                                 uint8_t requested) {
    if (!ino) return false;

    /* Kernel always has access */
    if (uid_is_kernel(uid)) {
        return true;
    }

    /* Owner (uid 0 = mdman/root) always has access */
    if (uid == 0 || ino->uid == 0) {
        return true;
    }

    /* Get ACL from inode */
    mdfs_acl_t acl;
    mdfs_inode_get_acl(ino, &acl);

    /* Build complete group list (primary gid + supplementary groups) */
    uint16_t all_groups[33];
    int all_group_count = 0;

    /* Add primary group */
    all_groups[all_group_count++] = (uint16_t)(gid & 0xFFFF);

    /* Add supplementary groups */
    for (int i = 0; i < group_count && all_group_count < 33; i++) {
        /* Avoid duplicates */
        bool duplicate = false;
        for (int j = 0; j < all_group_count; j++) {
            if (all_groups[j] == groups[i]) {
                duplicate = true;
                break;
            }
        }
        if (!duplicate) {
            all_groups[all_group_count++] = groups[i];
        }
    }

    /* Check ACL permissions */
    if (acl.ace_count > 0) {
        return mdfs_acl_check_permission(&acl, (uint16_t)(uid & 0xFFFF), 
                                        all_groups, all_group_count, requested);
    }

    /* Fallback: Traditional Unix-style permission check based on mode field
     * For now, if no ACL exists and not owner, grant read access only
     */
    if (ino->uid == uid) {
        /* Owner has full access */
        return true;
    } else if (gid == ino->gid) {
        /* Group member: read + execute */
        return (requested & ~(MDFS_PERM_READ | MDFS_PERM_EXECUTE)) == 0;
    } else {
        /* Others: read only */
        return (requested == MDFS_PERM_READ);
    }
}

void mdfs_inode_init_acl(mdfs_inode_t *ino, uint32_t owner_uid, uint32_t owner_gid) {
    if (!ino) return;

    mdfs_acl_t acl;
    mdfs_acl_create_default(&acl, (uint16_t)(owner_uid & 0xFFFF), 
                           (uint16_t)(owner_gid & 0xFFFF));
    mdfs_inode_set_acl(ino, &acl);
}
