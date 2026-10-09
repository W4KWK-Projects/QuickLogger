#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "models.hpp"

namespace ql
{

    class Database;

    // What a user may do on one net. Everyone can watch; a full user given
    // the net can log it; a Net Admin given the net, or an Admin, looks
    // after it.
    enum class NetPower
    {
        // Watch its open session, look at and export its history.
        kWatch,
        // Log a session, import and export, add and edit saved stations.
        kLog,
        // Everything on the net: change its details, delete it, its
        // sessions and check-ins in History, remove saved stations.
        kManage,
    };

    // What one user may do, worked out from the database (LoadUserAccess).
    // Plain data with no reference to the screen, so the UI, the push
    // commands and the tests all ask the same questions.
    //
    // Restricted-nets mode is off by default: then any full user logs and
    // changes any net, as before. When it is on, a recurring net can only
    // be touched by someone it was given to; ad hoc nets are exempt either
    // way. Nobody loses the ability to see what a view-only user sees.
    struct UserAccess
    {
        // Blank at the local console.
        std::string username;
        // The console is always an Admin.
        bool console = false;
        bool view_only = false;
        // kAccessUser, kAccessNetAdmin or kAccessAdmin.
        int level = kAccessUser;
        // The server option kOptionRestrictedNets.
        bool restricted = false;
        // The nets given to this user, by Database::GrantNet.
        std::vector<std::int64_t> grants;

        bool IsAdmin() const;
        bool IsNetAdmin() const;
        bool IsGranted(std::int64_t net_id) const;
        NetPower PowerOn(const Net& net) const;
        // New recurring nets (F2, and importing one that is new): anyone
        // who isn't view-only, unless restricted, then Net Admins and
        // Admins.
        bool CanCreateNets() const;
        // Whether this user may give `net` to a user of level
        // `target_level`: an Admin any net to a full user or a Net Admin; a
        // Net Admin a net they look after to a full user.
        bool CanGrant(const Net& net, int target_level) const;
        // Whether there is any recurring net this user can log: for
        // hiding Import from someone with nothing to import into.
        bool HasAnyGrant() const;
    };

    // `username`'s access now; a blank one is the console.
    UserAccess LoadUserAccess(Database* db, const std::string& username);

    // The words for a refusal: "Only Net Admins can edit nets." or "Tuesday
    // Net isn't one of your nets."
    std::string NetPowerRefusal(NetPower needed, const std::string& net_name, const std::string& what);

}  // namespace ql
