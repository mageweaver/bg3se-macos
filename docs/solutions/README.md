# Solution Documentation Guide

This directory contains comprehensive documentation of reverse-engineering solutions, architectural patterns, and implementation approaches for BG3SE-macOS.

## Documentation Structure

### Solution Documents

Solution documents combine YAML frontmatter (metadata) with detailed technical content:

```
---
title: "[Problem]: [Solution Description]"
date: YYYY-MM-DD
category: reverse-engineering|architecture|feature|bug-fix|investigation
component: Ext.ComponentName
issue: "#NUMBER"
severity: blocker|high|medium|low|documentation|architecture
tags:
  - technique
  - domain
  - component
---

# Content...
```

## Quick Start

### For Writers (Creating New Solutions)

1. **Copy the template**
   ```bash
   cp _FRONTMATTER_TEMPLATE.yaml your-solution.md
   ```

2. **Consult the reference**
   - Read `_FRONTMATTER_REFERENCE.md` for field guidance
   - Use decision matrices to choose appropriate values

3. **Follow the example structure**
   - Reference: `/reverse-engineering/ext-staticdata-implementation.md`
   - Use same sections: Problem, Investigation, Solution, Results, Debugging

4. **Quality checklist**
   - Check YAML syntax validity
   - Verify all file paths are absolute
   - Include code snippets (not full files)
   - Link to related issues
   - Document offsets in table format

### For Readers (Finding Solutions)

#### By Technique
```bash
# Find all Frida-based solutions
grep -r "- frida" .

# Find all Ghidra analysis
grep -r "- ghidra" .

# Find all pattern-scanning approaches
grep -r "- pattern-scanning" .
```

#### By Component
```bash
# Find all Ext.StaticData solutions
grep -r "^component: Ext.StaticData" .

# Find all Entity system solutions
grep -r "^component: Ext.Entity" .
```

#### By Severity
```bash
# Find all blocker-level solutions
grep -r "^severity: blocker" .

# Find architectural patterns
grep -r "^severity: architecture" .
```

#### By Issue
```bash
# Find solutions for specific issue
grep -r "^issue: \"#40\"" .

# Find all solutions that block other work
grep -r "^blocks:" . | grep -v "null"
```

## Frontmatter Reference Files

### `_FRONTMATTER_TEMPLATE.yaml`
**Use for:** Starting new solution documents
- Pre-filled with all required and optional fields
- Includes placeholder text and guidelines
- Points to examples and decision matrices

### `_FRONTMATTER_REFERENCE.md`
**Use for:** Understanding and filling frontmatter fields
- Detailed field-by-field documentation
- Decision matrices for each field
- Tag categorization and selection guide
- Search/filtering patterns
- Complete examples

## Example Solutions

### Ext.StaticData Implementation
**File:** `/reverse-engineering/ext-staticdata-implementation.md`

Demonstrates:
- File-based capture pattern
- 3-layer architecture (Frida → C → Lua)
- ARM64 ABI considerations
- Why Dobby hooks failed (PC-relative corruption)
- Debugging workflow with Frida

**Tags:** `arm64`, `frida`, `file-based-capture`, `architecture`

### StaticData FeatManager Discovery (Previous Attempt)
**File:** `/reverse-engineering/staticdata-featmanager-discovery.md`

Documents:
- Initial investigation path
- TypeContext limitations
- Dobby hook failure analysis
- Alternative approaches considered
- Prevention patterns

**Tags:** `reverse-engineering`, `arm64`, `hooking`, `investigation`

## Frontmatter Fields Explained

### Required Fields

| Field | Purpose | Example |
|-------|---------|---------|
| `title` | Searchable problem/solution description | "TypeContext Returns Metadata: Solved via Frida" |
| `date` | When solution was finalized | `2025-12-15` |
| `category` | Type of solution | `reverse-engineering` |
| `component` | Affected BG3SE component | `Ext.StaticData` |
| `issue` | GitHub issue number(s) | `"#40"` or `"#40, #41"` |
| `severity` | Business impact | `architecture` |
| `tags` | Array of searchable keywords | `[arm64, frida, file-based-capture]` |

### Optional Fields

| Field | Purpose | Example |
|-------|---------|---------|
| `parent_issue` | Broader tracking issue | `"#33"` (static data tracking) |
| `blocked_by` | Blocking dependencies | `["#44", "#15"]` |
| `blocks` | Issues this unblocks | `["#41"]` |
| `related_offsets` | Key memory addresses | `["0x01b752b4: GetFeats"]` |

## Document Sections (Recommended Structure)

1. **Summary** - One paragraph with key outcomes
2. **Problem** - What failed, why it failed
3. **Technical Investigation** - Discovery path, attempts, learnings
4. **Solution Architecture** - How it works, design decisions
5. **Implementation Details** - Code, structure, offsets
6. **Key Offsets** - Table of discovered addresses
7. **Data Structures** - C struct definitions
8. **Workflow** - How to use the solution
9. **Results** - Quantifiable outcomes
10. **Debugging** - How to verify it's working
11. **References** - Links to issues, code, related docs

## Tag Categories

### Technique Tags
- `frida` - Frida runtime instrumentation
- `dobby` - Dobby inline hooking
- `ghidra` - Ghidra decompilation
- `pattern-scanning` - Binary pattern matching
- `symbol-resolution` - dlsym / symbol lookup

### Architecture Tags
- `hook-based` - Function hooking
- `file-based-capture` - File I/O integration
- `singleton-discovery` - Finding managers
- `type-context` - Type reflection system
- `interceptor` - Non-invasive interception

### Domain Tags
- `arm64` - ARM64-specific
- `memory-layout` - Struct offset discovery
- `calling-convention` - ARM64 ABI
- `pc-relative` - PC-relative instructions

### Component Tags
- `staticdata` - Static data system
- `entity-system` - ECS
- `stats-system` - Stats system
- `prototype-managers` - Spell/Status/Passive managers

## YAML Syntax Checklist

```yaml
---
# String (required)
title: "Your title here"

# Date (required) - ISO 8601
date: 2025-12-15

# Single string (required) - not array
category: reverse-engineering

# Single string (required)
component: Ext.StaticData

# String with # prefix (required)
issue: "#40"

# Single string (required)
severity: architecture

# Array with dashes (required)
tags:
  - arm64
  - frida

# String or null (optional)
parent_issue: "#33"

# Array or null (optional)
blocked_by: ["#44", "#15"]

# Array or null (optional)
blocks: ["#41"]

# Array of "0xADDR: Description" (optional)
related_offsets:
  - "0x01b752b4: GetFeats"
  - "0x7C: Count offset"
---
```

## File Organization

```
docs/solutions/
├── README.md                              # This file
├── _FRONTMATTER_TEMPLATE.yaml             # Template for new docs
├── _FRONTMATTER_REFERENCE.md              # Field documentation
├── reverse-engineering/
│   ├── ext-staticdata-implementation.md   # Complete solution
│   ├── staticdata-featmanager-discovery.md # Investigation notes
│   └── ...
├── architecture/
│   └── ...
├── features/
│   └── ...
└── bug-fixes/
    └── ...
```

## Best Practices

### Writing Effective Solutions

1. **Title as Query**
   - Make it searchable: "FeatManager NOT accessible via TypeContext"
   - Include solution approach: "Solved via Frida File Capture"
   - Avoid vague: "Solution for Issue #40"

2. **Document-as-You-Go**
   - Write while solving, not after
   - Include investigation dead-ends (why they failed)
   - Explain decision points

3. **Technical Depth**
   - Include C struct definitions
   - Show ARM64 assembly patterns
   - Provide offset tables
   - Include code snippets (not full files)

4. **Practical Focus**
   - How to reproduce
   - How to verify it's working
   - Common issues and fixes
   - Debug commands

5. **Metadata Discipline**
   - Use exact component names from codebase
   - Link all related issues
   - Tag for future discovery
   - Update severity if broader impact found

### Reading Solutions

1. **Quick Scan**
   - Read title, summary, results section
   - Check if it applies to your work

2. **Deep Dive**
   - Follow investigation path
   - Study architecture diagram
   - Review key offsets
   - Try debugging workflow

3. **Cross-Reference**
   - Check related issues (blocks/blocked_by)
   - Look at alternative approaches
   - Review similar component solutions

## Maintenance

### When to Update

- **New discovery:** Add related_offsets
- **Dependency changes:** Update blocked_by/blocks
- **Alternative found:** Add to solution alternatives
- **Implementation complete:** Update severity/tags

### When to Create New

- **Different component:** New document
- **Different technique:** New document (e.g., Frida vs Dobby)
- **Different game version:** Separate or version-note
- **Investigation vs solution:** Separate documents

## Contributing

### Adding a New Solution

1. Create file: `solutions/[category]/[title].md`
2. Copy frontmatter template
3. Fill out required fields
4. Write content following recommended sections
5. Include code snippets, diagrams, offsets
6. Link to related issues/components
7. Validate YAML syntax
8. Commit with clear message

### Updating Existing Solution

1. Update date only if solution fundamentally changes
2. Add new offsets to related_offsets
3. Update blocks/blocked_by if dependencies change
4. Update severity if scope increases
5. Add new tags if approach applies more broadly

## Tools and Resources

### For YAML Validation
```bash
# Validate YAML syntax
python3 -c "import yaml; yaml.safe_load(open('file.md'))"

# Or use online validator
# https://www.yamllint.com/
```

### For Searching
```bash
# Find solutions mentioning specific technique
grep -r "- frida" docs/solutions/

# Find solutions by severity
grep "^severity: " docs/solutions/**/*.md | sort

# Find all offsets discovered
grep "related_offsets:" -A 10 docs/solutions/**/*.md
```

### For Analysis
```bash
# Count solutions by category
grep "^category:" docs/solutions/**/*.md | cut -d: -f2 | sort | uniq -c

# Find blocking dependencies
grep "blocks:" docs/solutions/**/*.md | grep -v "null"

# Find all related offsets
grep "0x[0-9a-f]" docs/solutions/**/*.md | grep related_offsets -A 20
```

## Related Documentation

- **ghidra/offsets/** - Offset documentation organized by system
- **agent_docs/meridian-persona.md** - RE session approach
- **agent_docs/development.md** - Development workflow

## Questions?

For questions about:
- **YAML frontmatter:** See `_FRONTMATTER_REFERENCE.md`
- **Document structure:** See example solutions
- **Technique details:** Search by tag or component
- **Related issues:** Check issue link in frontmatter
