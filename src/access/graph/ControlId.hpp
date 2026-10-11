#pragma once

// The Graph A11y Kernel — engine-neutral accessibility navigation core.
// Lifted verbatim from CyberAccess src/Graph/ (spec: docs/graph-a11y-spec.md). Lineage: Factorio
// Access key-graph.lua/menu.lua → Tanglebeep (ported with permission) → WrathAccess → RTAccess →
// CyberAccess → Sims2Access → OpenXcom → this port (orion2re). Keep it byte-for-byte close to upstream so fixes
// can flow both ways. Deltas: the namespace (access::graph), and C++17 spellings where upstream uses C++20.
// This directory is a lifted component and keeps upstream style, including logic_error throws on builder
// misuse; host code catches them at the one Build call site (src/access/navigator.cpp).
//
// This directory is STL-only by design: NO <windows.h>, no game headers. The conformance tests
// compile Graph/** standalone, which is what enforces the kernel/host boundary.

#include <cstddef>
#include <cstdio>
#include <functional>
#include <ostream>
#include <string>

namespace access::graph
{

/// The identity of a control (graph node) — a two-tier identity so focus can be followed across
/// rebuilds even when the world shifts under us.
///
/// Reference (optional) is the game/domain object a node was derived from (a widget instance, an
/// item), compared by POINTER IDENTITY — the kernel never dereferences it. StructuralKey (required
/// for a valid id) is a value-equatable key — a string, or a composite formatted into one
/// ("grid/3/2"). Structural keys must be stable across rebuilds for as long as the control
/// logically exists.
///
/// Two controls are "the same" when their references are identical (tier 1 — a perfect match that
/// follows an object that MOVED, its structural key changing) OR their structural keys are equal
/// (tier 2 — follows a logical control whose backing object was rebuilt: new instance, same
/// identity).
///
/// Equality/hashing is defined on StructuralKey ALONE, so a ControlId is a stable map key (the
/// graph stores nodes and traversal order by it). The reference tier is metadata, applied
/// explicitly during focus reconciliation via ReferenceMatches.
///
/// A default-constructed ControlId (empty key) is the null/no-id sentinel: IsValid() == false.
class ControlId
{
public:
    /// The originating game/domain object, or nullptr. Matched by pointer identity only.
    const void* Reference = nullptr;

    /// The value-equatable structural identity. Empty = the invalid/null id.
    std::string StructuralKey;

    ControlId() = default;

    /// A control identified only by a structural key (no backing object).
    static ControlId Structural(std::string structuralKey)
    {
        ControlId id;
        id.StructuralKey = std::move(structuralKey);
        return id;
    }

    /// A control with both tiers: a backing object and a structural key.
    static ControlId Referenced(const void* reference, std::string structuralKey)
    {
        ControlId id;
        id.Reference = reference;
        id.StructuralKey = std::move(structuralKey);
        return id;
    }

    /// A control identified by a backing object only — the object doubles as its own structural
    /// key (equality collapses to identity). For wrapping a raw widget with no better key.
    static ControlId ForObject(const void* reference)
    {
        char buf[32];
        std::snprintf(buf, sizeof(buf), "obj:%p", reference);
        return Referenced(reference, buf);
    }

    bool IsValid() const { return !StructuralKey.empty(); }

    /// Tier-1 test: is `obj` this control's backing object?
    bool ReferenceMatches(const void* obj) const { return Reference != nullptr && Reference == obj; }

    bool operator==(const ControlId& other) const { return StructuralKey == other.StructuralKey; }
    bool operator!=(const ControlId& other) const { return !(*this == other); }

    std::string ToString() const
    {
        if (!Reference)
            return "ControlId(" + StructuralKey + ")";
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%p", Reference);
        return "ControlId(" + StructuralKey + ", ref=" + buf + ")";
    }

    friend std::ostream& operator<<(std::ostream& os, const ControlId& id) { return os << id.ToString(); }
};

} // namespace access::graph

template <>
struct std::hash<access::graph::ControlId>
{
    std::size_t operator()(const access::graph::ControlId& id) const noexcept
    {
        return std::hash<std::string>{}(id.StructuralKey);
    }
};
