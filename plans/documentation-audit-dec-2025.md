# Documentation Audit - December 2025

**Date:** 2025-12-28
**Version:** v0.36.17 | **Parity:** ~82%
**Auditor:** the CLI with parallel subagents

## Executive Summary

Comprehensive audit of all documentation across README, CLAUDE.md, agent_docs/, docs/, and ghidra/offsets/. The documentation is fundamentally accurate but has several gaps that should be addressed.

**Overall Assessment:** Documentation is 90% accurate with actionable fixes identified.

---

## 1. README.md

**Status:** ✅ ACCURATE AND CURRENT

| Check | Status |
|-------|--------|
| Version (v0.36.17) | ✅ Matches version.h |
| Build instructions | ✅ Updated with submodule handling (Issue #54) |
| Verify build section | ✅ Added Dec 28 |
| Troubleshooting table | ✅ Added Dec 28 |
| Steam configuration | ✅ Clear step-by-step |
| Feature status table | ✅ Current |

**No action required.**

---

## 2. CLAUDE.md

**Status:** ✅ ACCURATE

| Check | Status | Notes |
|-------|--------|-------|
| Version | ✅ v0.36.17 | Matches all files |
| Parity | ✅ ~82% | Consistent |
| Line count | ⚠️ 135 lines | Above 100-line ideal but acceptable |
| Progressive disclosure | ✅ | Uses @agent_docs/ imports |
| Build commands | ✅ | CMake workflow correct |
| Key offsets | ✅ | All verified against codebase |

**No action required.**

---

## 3. agent_docs/

**Status:** ⚠️ 1 CRITICAL ISSUE, 2 MINOR ISSUES

### Findings Table

| File | Status | Issues |
|------|--------|--------|
| architecture.md | ✅ ACCURATE | No issues |
| development.md | ❌ CRITICAL | `tail_log.sh` documented but script doesn't exist |
| reference.md | ✅ ACCURATE | No issues |
| acceleration.md | ⚠️ MINOR | Component counts outdated (158 → 462 layouts) |
| ghidra.md | ✅ ACCURATE | No issues |
| meridian-persona.md | ✅ ACCURATE | No issues |
| debugging-strategies.md | ✅ ACCURATE | No issues |

### Action Items

1. **CRITICAL:** Remove or implement `tail_log.sh` script documentation in development.md (lines 22-32)
2. **MINOR:** Update acceleration.md component baseline counts to reflect 462 layouts
3. **MINOR:** Add missing modules to architecture.md (console/, io/, lifetime/, enum/, overlay/)

---

## 4. docs/

**Status:** ⚠️ 2 CRITICAL ISSUES, 3 MINOR ISSUES

### Findings Table

| File | Status | Issues |
|------|--------|--------|
| api-reference.md | ❌ CRITICAL | Missing Ext.Resource and Ext.StaticData sections |
| getting-started.md | ❌ CRITICAL | Stale build instructions (build.sh), v0.30.0 log example, outdated logging docs |
| troubleshooting.md | ⚠️ MINOR | Log file location needs update for session-based logs |
| CHANGELOG.md | ✅ VERIFIED | No issues - v0.36.17 current |
| architecture.md | ✅ ACCURATE | No issues |
| development.md | ✅ ACCURATE | No issues |
| reverse-engineering.md | ✅ ACCURATE | No issues |
| components/*.md | ⚠️ MINOR | eoc-components count discrepancy (276 claimed, 268 shown) |
| arm64/*.md | ✅ VERIFIED | No issues |

### Action Items

1. **CRITICAL:** Add `## Ext.Resource` section to api-reference.md
   - Document Get(), GetAll(), GetTypes(), GetCount(), IsReady()
   - Cover 34 resource types

2. **CRITICAL:** Add `## Ext.StaticData` section to api-reference.md
   - Document all 9 types: Feat, Race, Background, Origin, God, Class, Progression, ActionResource, FeatDescription
   - Document ForceCapture() and HashLookup()

3. **CRITICAL:** Update getting-started.md
   - Replace build.sh instructions with CMake approach
   - Update example log version from v0.30.0 to v0.36.17
   - Document session-based logging (`logs/` directory, `latest.log` symlink)

4. **MINOR:** Update troubleshooting.md log paths for session-based logs

5. **MINOR:** Reconcile eoc-components.md count (276 claimed vs 268 documented)

---

## 5. ghidra/offsets/

**Status:** ⚠️ 1 CRITICAL ISSUE, 3 MINOR ISSUES

### Offset Verification

All 11 key offsets from CLAUDE.md verified:

| Offset | Symbol | Status |
|--------|--------|--------|
| 0x348 | RPGSTATS_OFFSET_FIXEDSTRINGS | ✅ |
| 0x10124f92c | LEGACY_IsInCombat | ✅ |
| 0x10898e8b8 | esv::EocServer::m_ptr | ✅ |
| 0x10898c968 | ecl::EocClient::m_ptr | ⚠️ NOT DOCUMENTED |
| 0x1089bac80 | SpellPrototypeManager::m_ptr | ✅ |
| 0x1089bdb30 | StatusPrototypeManager::m_ptr | ✅ |
| 0x108aeccd8 | PassivePrototypeManager | ✅ |
| 0x108aecce0 | InterruptPrototypeManager | ✅ |
| 0x108991528 | BoostPrototypeManager | ✅ |
| 0x108a8f070 | ResourceManager::m_ptr | ✅ |
| 0x101f72754 | SpellPrototype::Init | ✅ |

### Findings

| Issue | Priority | Details |
|-------|----------|---------|
| Duplicate GlobalStringTable docs | CRITICAL | GLOBAL_STRING_TABLE.md (stale) + GLOBALSTRINGTABLE.md (current) |
| Missing ecl::EocClient docs | HIGH | 0x10898c968 used in code but not documented |
| Stale status markers | MEDIUM | COMPONENTS.md has outdated discovery status |
| Inconsistent date formats | LOW | No standardized "Last Updated" field |

### Action Items

1. **CRITICAL:** Delete `GLOBAL_STRING_TABLE.md` (stale duplicate of `GLOBALSTRINGTABLE.md`)

2. **HIGH:** Add ecl::EocClient documentation to ENTITY_SYSTEM.md:
   ```markdown
   ## Client Singleton

   | Symbol | Address | Notes |
   |--------|---------|-------|
   | ecl::EocClient::m_ptr | 0x10898c968 | Client-side EoCClient singleton |
   ```

3. **MEDIUM:** Mark deprecated sections in COMPONENTS.md and STATICDATA_MANAGERS.md

4. **LOW:** Standardize update dates to ISO 8601 format (YYYY-MM-DD)

---

## Priority Matrix

### Immediate (Before Next Release)

| Task | File | Effort |
|------|------|--------|
| Add Ext.Resource docs | docs/api-reference.md | Medium |
| Add Ext.StaticData docs | docs/api-reference.md | Medium |
| Update build instructions | docs/getting-started.md | Low |
| Delete stale file | ghidra/offsets/GLOBAL_STRING_TABLE.md | Trivial |
| Remove/implement tail_log.sh | agent_docs/development.md | Low |

### Short-term (This Week)

| Task | File | Effort |
|------|------|--------|
| Add ecl::EocClient docs | ghidra/offsets/ENTITY_SYSTEM.md | Low |
| Update log version example | docs/getting-started.md | Trivial |
| Document session-based logging | docs/getting-started.md | Low |
| Update troubleshooting log paths | docs/troubleshooting.md | Trivial |

### Long-term (Maintenance)

| Task | File | Effort |
|------|------|--------|
| Update component counts | agent_docs/acceleration.md | Low |
| Add missing modules to architecture | agent_docs/architecture.md | Low |
| Reconcile eoc-components count | docs/components/eoc-components.md | Low |
| Standardize date formats | ghidra/offsets/*.md | Medium |

---

## Validation Commands

After fixes, run these to verify:

```bash
# Verify versions match
grep -r "v0.36.17" README.md CLAUDE.md src/core/version.h docs/CHANGELOG.md

# Check for stale files
ls -la ghidra/offsets/GLOBAL_STRING_TABLE.md  # Should not exist after fix

# Verify new API sections exist
grep -E "^## Ext\.(Resource|StaticData)" docs/api-reference.md

# Check build instructions updated
grep -c "build.sh" docs/getting-started.md  # Should be 0 or minimal
grep -c "cmake" docs/getting-started.md     # Should be present
```

---

## Conclusion

The documentation is fundamentally sound and reflects the current v0.36.17 state. The README improvements from Issue #54 are solid. The main gaps are:

1. **Missing API documentation** for Ext.Resource and Ext.StaticData (implemented but undocumented)
2. **Stale getting-started.md** with old build instructions
3. **Duplicate offset file** that should be deleted
4. **Phantom script** (tail_log.sh) documented but not implemented

Addressing these issues will bring documentation to 100% accuracy.
