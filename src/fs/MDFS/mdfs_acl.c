#include "moduos/fs/MDFS/mdfs_acl.h"
#include "moduos/kernel/memory/string.h"

bool mdfs_acl_check_permission(const mdfs_acl_t *acl,
                               uint16_t uid,
                               const uint16_t *gids,
                               int gid_count,
                               uint8_t requested) {
    if (!acl || acl->ace_count == 0) {
        /* No ACL means fall back to traditional permissions */
        return true;
    }

    uint8_t allowed_mask = 0;
    uint8_t denied_mask = 0;

    /* Iterate through all ACEs */
    for (int i = 0; i < acl->ace_count && i < MDFS_MAX_ACES; i++) {
        mdfs_ace_t ace = acl->aces[i];
        uint16_t ace_id = MDFS_ACE_GET_ID(ace);
        bool is_group_ace = MDFS_ACE_GET_IS_GROUP(ace);
        bool match = false;

        /* Check if ACE matches user or any of their groups */
        if (!is_group_ace && ace_id == uid) {
            match = true;
        } else if (is_group_ace && gids) {
            for (int g = 0; g < gid_count; g++) {
                if (ace_id == gids[g]) {
                    match = true;
                    break;
                }
            }
        }

        if (match) {
            uint8_t perms = MDFS_ACE_GET_PERMISSIONS(ace);
            if (MDFS_ACE_GET_TYPE(ace) == MDFS_ACE_DENY) {
                denied_mask |= perms;
            } else {
                allowed_mask |= perms;
            }
        }
    }

    /* Calculate final permissions: allowed AND NOT denied */
    uint8_t final_perms = allowed_mask & ~denied_mask;

    /* Check if all requested bits are present in final_perms */
    return (final_perms & requested) == requested;
}

void mdfs_acl_init(mdfs_acl_t *acl) {
    if (!acl) return;
    memset(acl, 0, sizeof(mdfs_acl_t));
}

int mdfs_acl_add_ace(mdfs_acl_t *acl, uint16_t id, bool is_group,
                     mdfs_ace_type_t type, uint8_t perms) {
    if (!acl || acl->ace_count >= MDFS_MAX_ACES) {
        return -1;
    }

    acl->aces[acl->ace_count] = MDFS_ACE_MAKE(id, is_group ? 1 : 0, type, perms);
    acl->ace_count++;
    return 0;
}

int mdfs_acl_remove_ace(mdfs_acl_t *acl, int index) {
    if (!acl || index < 0 || index >= acl->ace_count) {
        return -1;
    }

    /* Shift remaining ACEs down */
    for (int i = index; i < acl->ace_count - 1; i++) {
        acl->aces[i] = acl->aces[i + 1];
    }

    acl->ace_count--;
    acl->aces[acl->ace_count] = 0; /* Clear last entry */
    return 0;
}

void mdfs_acl_clear(mdfs_acl_t *acl) {
    if (!acl) return;
    acl->ace_count = 0;
    memset(acl->aces, 0, sizeof(acl->aces));
}

void mdfs_acl_create_default(mdfs_acl_t *acl, uint16_t owner_uid, uint16_t owner_gid) {
    if (!acl) return;
    
    mdfs_acl_init(acl);

    /* Allow owner full permissions */
    uint8_t owner_perms = MDFS_PERM_READ | MDFS_PERM_WRITE | 
                         MDFS_PERM_EXECUTE | MDFS_PERM_DELETE | 
                         MDFS_PERM_OWNERSHIP;
    mdfs_acl_add_ace(acl, owner_uid, false, MDFS_ACE_ALLOW, owner_perms);

    /* Allow owner's group read and execute */
    uint8_t group_perms = MDFS_PERM_READ | MDFS_PERM_EXECUTE;
    mdfs_acl_add_ace(acl, owner_gid, true, MDFS_ACE_ALLOW, group_perms);
}
