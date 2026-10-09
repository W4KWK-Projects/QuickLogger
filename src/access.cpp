#include "access.hpp"

#include <algorithm>

#include "db/database.hpp"

namespace ql
{

    bool UserAccess::IsAdmin() const
    {
        return console || level >= kAccessAdmin;
    }

    bool UserAccess::IsNetAdmin() const
    {
        return console || level >= kAccessNetAdmin;
    }

    bool UserAccess::IsGranted(std::int64_t net_id) const
    {
        return std::find(grants.begin(), grants.end(), net_id) != grants.end();
    }

    NetPower UserAccess::PowerOn(const Net& net) const
    {
        if (view_only)
        {
            return NetPower::kWatch;
        }
        if (IsAdmin() || net.is_ad_hoc || !restricted)
        {
            return NetPower::kManage;
        }
        if (!IsGranted(net.id))
        {
            return NetPower::kWatch;
        }
        return IsNetAdmin() ? NetPower::kManage : NetPower::kLog;
    }

    bool UserAccess::CanCreateNets() const
    {
        return !view_only && (!restricted || IsNetAdmin());
    }

    bool UserAccess::CanGrant(const Net& net, int target_level) const
    {
        if (view_only || net.is_ad_hoc)
        {
            return false;
        }
        if (IsAdmin())
        {
            return target_level == kAccessUser || target_level == kAccessNetAdmin;
        }
        return level == kAccessNetAdmin && IsGranted(net.id) && target_level == kAccessUser;
    }

    bool UserAccess::HasAnyGrant() const
    {
        return !grants.empty();
    }

    UserAccess LoadUserAccess(Database* db, const std::string& username)
    {
        UserAccess access;
        access.username = username;
        access.console = username.empty();
        if (db == nullptr)
        {
            return access;
        }
        // One snapshot for all of it.
        Database::ReadTransaction reads(db);
        access.restricted = db->ServerOptionOn(kOptionRestrictedNets);
        if (access.console)
        {
            access.level = kAccessAdmin;
            return access;
        }
        access.view_only = db->IsUserViewOnly(username);
        access.level = db->GetUserAccessLevel(username);
        access.grants = db->GetNetGrants(username);
        return access;
    }

    std::string NetPowerRefusal(NetPower needed, const std::string& net_name, const std::string& what)
    {
        if (needed == NetPower::kManage)
        {
            return "Only Net Admins can " + what + ".";
        }
        return (net_name.empty() ? std::string("That net") : net_name) + " isn't one of your nets.";
    }

}  // namespace ql
