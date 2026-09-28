# Version numbers

QuickLogger's version is **major.minor.patch** (for example 1.7.2), set in `CMakeLists.txt`'s `project()` line and tagged `v` plus the version. Which number goes up depends on what a release does for the people using it, judged by the release as a whole, not by each change in it.

## Patch (1.7.0 → 1.7.1)

The default. Anything that makes QuickLogger work better without changing how anyone uses it, or changes it only in a small way:

- Bug fixes, security fixes and performance improvements.
- Small improvements to something that already exists: wording, labels, key bar layout, alignment, a clearer error message, a new column or detail shown, a new key for something a page can already do.
- New download platforms, build and packaging changes, server setup (`deploy/`) and documentation.

A patch release never changes the database schema or a file format, so going back to the previous patch release is always safe.

## Minor (1.7.x → 1.8.0)

A release worth announcing: something new that a user would want to be told about, or a change they have to relearn. Any one of these makes it minor:

- A new feature: a new page or window, a new workflow (such as importing a session), a new kind of file to export or import, a new per-net or per-user setting.
- A change to how an existing feature works that users will notice and need to know about.
- **Any database schema change.** Upgrading the database is one-way, so a release that changes it can't be a patch.

Smaller changes to user functionality can wait on main and go out with the next minor release, or go out sooner as a patch if they meet the patch rules above.

## Major (1.x → 2.0.0)

Something existing users or servers have to act on: a removed feature, a file or database from before that can no longer be read, a platform no longer supported, or a server that needs its setup redone.

## In practice

- When unsure between patch and minor, ask: would this go in the release notes' "What's new" as something users should know about, or under fixes and improvements? The first is minor, the second patch.
- Several small user-facing improvements can go out together in a patch release. Save the minor number for when there's something to announce.
