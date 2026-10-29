# Material node clipboard delivery

One delivery represents one visible change. Prepare nodes and validation in memory,
then copy native UE graph text. The user chooses where to Ctrl+V and which existing
wires to connect. No production material is edited, saved, or replaced.

## Actions

`material.copy_material_nodes` takes:

- `summary`: what this delivery does.
- `graph_spec`: the existing `nodes`, `custom_hlsl_nodes`, `connections` schema.
  Use unique nonempty `id`s, numeric JSON values for numeric properties, and only
  group-internal expression connections. 1–128 nodes per delivery.
  For Named Reroutes, also provide `graph_spec.named_reroutes` as an array of
  `{"usage":"usage_node_id","declaration":"declaration_node_id"}`. Create
  ordinary `NamedRerouteDeclaration` nodes (`props.Name` is the display label)
  and `NamedRerouteUsage` nodes without a `Declaration` property. Bind by local
  node ID, not by display name or asset path. Every Usage needs exactly one
  binding. Multiple usages can reference the same declaration. An external
  declaration input can use `pin:""`. Dependency/cycle checks follow these
  implicit links as well as ordinary input wires.
- `external_inputs`: `[{node,pin,name,sample:[numbers]}]`. Each endpoint must be
  unconnected. Samples contain 1–4 finite components; they validate the graph but
  are not included in the clipboard. Material-dependent texture inputs should be
  supplied as actual existing texture object nodes in the group.
- `outputs`: `[{node,pin,name,property?}]`. Each output is compiled separately.
  Default property `EmissiveColor` uses a temporary numeric adapter for Float1–4.
  Other supported properties: `BaseColor`, `Normal`, `Roughness`,
  `WorldPositionOffset`. Choose the property matching the intended use.
- `context_asset_path` (optional): copies the base material's domain, blend mode,
  shading model and two-sided flag into the transient validation material.
  It never edits the context asset or its open preview copy.

The result separately reports `sample_compile_passed`, `native_roundtrip_passed`,
`clipboard_written`, `destination_modified`, `pasted_visual_result_verified`,
plus dependencies and connection instructions. A successful preparation is not
proof of the final appearance after the user connects the nodes.

`material.recopy_last_material_nodes` rewrites the latest successful delivery in
this UE session. No files, clipboard history, or background application are needed.
Failure leaves the successful delivery cache intact. Validation failures never
touch the clipboard. Windows writes hold a single clipboard lock across snapshot,
write and readback. On failure the saved formats are restored; restoration status
is reported. If an existing format cannot be backed up safely (for example an
owner-display or enhanced-metafile handle), preparation fails before clearing it.
Text, HTML, bitmap and memory-backed registered formats use native Windows copies.
See [OleDuplicateData](https://learn.microsoft.com/en-us/windows/win32/api/ole2/nf-ole2-oleduplicatedata).
Clipboard failure never updates the last successful delivery.

## First use

Send `Templates/MaterialClipboard/HighlightLookupUV.json` as the action parameters.
It produces an editable Custom node with four external inputs and raw positive UV:

- Normalize explicit strand, viewer and light directions.
- U = unpowered strand highlight response × MaskG, saturated.
- V = squared absolute strand/view dot product.
- Zero direction inputs return (0,0); opposite light/view returns U=0 with V kept.
- Vertical flips, coordinate offsets, Power and final coloring remain external.

Paste into a normal material graph. Connect the four named inputs, then connect
the output to the existing highlight lookup texture's UVs. Inspect the sampled RGB
before multiplying it into the current highlight. The delivery creates no MF asset.

## Validation and limits

Preparation uses native `PrepareForCopying`, `ExportNodesToText`, `PostCopyNode`
and a second transient graph's `ImportNodesFromText`, followed by material
expression registration and `LinkMaterialExpressionsFromGraph`. Roundtrip checks editable
expression properties (including HLSL), pin names, internal links and layout.
Named-reference topology is compared explicitly, including non-editable reference
properties. Imported expressions undergo material-expression `PostCopyNode`
fixup; each Usage must reference a declaration inside the imported set with a
matching GUID. Results include `named_reroute_binding_count` and
`named_reroute_ownership_passed`.
Sample nodes and root connections are removed before export. UE owns actual
paste positioning, new identifiers, UI updates and the paste undo transaction.

V1 covers flat ordinary material graphs. Function input/output and composite
subgraphs are not supported. Existing texture/function assets are referenced,
not bundled; moving the text to another project requires those assets there.
The sample compiler is not an exhaustive rendering test for every parameter or
static-switch permutation. Run `Monolith.Material.Clipboard` automation tests and
perform the native editor Ctrl+V / twice-paste / undo check when changing this code.

Development uses the existing Monolith folder and build. Keep UE 5.7 compatible
APIs; report 5.7 untested if only the project's UE 5.8 toolchain is available.

## Repeatable GPU sample validation

Run in the UE Python console (the script resolves its template relative to itself):

```python
import runpy, unreal
suite = runpy.run_path(unreal.Paths.project_plugins_dir() + "Monolith/Tests/validate_material_clipboard_uv.py")
print(suite["run"]())
# Optional: run(existing_lookup_texture_path) also checks view-dependent RGB.
```

The script compiles the shipped HLSL into a transient ordinary material, renders
to a floating-point target and reads back ten numeric cases. Passing an existing
lookup texture additionally checks four view directions. It creates no saved assets.
Every computational node in a delivery must reach a declared output; comments
are exempt. Duplicate input connections and cycles are rejected.

## Local acceptance record — 2026-09-06

- UE 5.8 Development Editor full DLL build succeeded (not only Live Coding).
- `Monolith.Material.Clipboard`: 4/4 passed. Covers ordinary + Custom + comment
  native roundtrip, invalid inputs/properties, missing dependency, bad HLSL,
  forced native clipboard rollback, shipped UV template and successful-cache
  preservation followed by recopy. Native import emits reference-resolution
  warnings before graph-link synchronization; the semantic comparison passes.
- Existing project material-function and Lookup-texture references each passed
  compilation, native roundtrip and clipboard readback.
- The GPU script passed all ten cases and four view-dependent Lookup RGB samples.
- Native Ctrl+V produced an editable four-input Custom node. A second Ctrl+V
  increased expression count from one to two; one Ctrl+Z returned it to one.
- No `MonolithClipboard` assets were created in Content. The final clipboard
  text was checked against the shipped raw-UV formula.

Incomplete acceptance: UE 5.7 was not run. The final Hair material's wiring and
appearance remain for the destination integration check. A ShaderPreprocessor
shared-pointer assertion terminated the live editor during acceptance; its root
cause remains unresolved. A full rebuild and subsequent separate automated run
passed. Ten recovered project assets were checked against the preserved autosaves;
recovery copies reside under the project's Saved directory. This record does not
claim an uninterrupted or fully completed end-to-end acceptance.

The automation `TemplateAndCache` test intentionally leaves the shipped UV node
in the clipboard. Its recopy cache lasts only as long as that editor process;
after restarting UE, generate a fresh delivery before asking to recopy it.
