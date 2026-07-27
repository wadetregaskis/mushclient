# List controls on the port: why column order is not implemented

**Status:** decision recorded 2026-07-26. Revisit only for the reasons in §7.

**Short version:** the port maps the canonical `SysListView32` to `wxListCtrl`,
which on macOS *is* `wxGenericListCtrl`, whose column-reordering API is an
unimplemented stub in the shipped library. Saved column **order** therefore does
not round-trip. Column **widths** and the **sort column** do (`b16ea8c`). The
fix is a control migration, not a shim edit, and the cost falls on thirteen
dialogs that do not want the feature. We chose not to pay it.

---

## 1. What the setting is

MUSHclient persists three things per list into the `control` table of the prefs
database, via `CMUSHclientApp::SaveColumnConfiguration` /
`RestoreColumnConfiguration` (`MUSHclient.cpp:1175` / `:1230`):

| key | status on the port |
|---|---|
| `"<list> Col N Width"` | works (`b16ea8c`) |
| `"<list> Sort Sequence"` + `"<list> Sort Reverse"` | works (`b16ea8c`) |
| `"<list> Col N Order"` | **does not work — this document** |

Six canonical call sites move the order through the two Win32 messages
`LVM_GETCOLUMNORDERARRAY` / `LVM_SETCOLUMNORDERARRAY`:

* `MUSHclient.cpp:1196` — the save side, for every list
* `dialogs/world_prefs/genpropertypage.cpp:1563` — the five entity pages
* `dialogs/world_prefs/prefspropertypages.cpp:2403` — the macro list
* `dialogs/plugins/PluginsDlg.cpp:117` — the Plugins manager
* `dialogs/chat/ChatListDlg.cpp:173` — the chat list
* `ActivityView.cpp:118` — the Activity window

## 2. Why it cannot simply be bridged

`platform/wx_mfc_impl.cpp`'s `hook_send_message` would map those two messages to
`wxListCtrl::Get/SetColumnsOrder`. Both exist and are exported, so an API-level
check says "supported". Here is what is actually in the shipped
`libwx_osx_cocoau_core-3.3.2.0.0.dylib`:

```
__ZN17wxGenericListCtrl15SetColumnsOrderERK10wxArrayInt:
   mov  w0, #0x0
   ret

__ZNK17wxGenericListCtrl15GetColumnsOrderEv:
   stp  xzr, xzr, [x8]        ; zero the returned wxArrayInt
   str  xzr, [x8, #0x10]
   ret
```

`SetColumnsOrder` returns `false` and does nothing; `GetColumnsOrder` returns an
empty array. They are declared `override` and never implemented.

**There is no fallback.** `wx/listctrl.h` selects a native `wxListCtrl` only for
`__WXMSW__` and `__WXQT__`; every other port, macOS included, gets
`wx/generic/listctrl.h`, where `class wxListCtrl : public wxGenericListCtrl`. On
this platform the stub *is* the control.

The user-facing half is missing for the same reason. The generic list builds its
header from its own private `wxListHeaderWindow`, whose entire exported surface
is `OnMouse` / `OnPaint` / `DrawCurrent` / `SendListEvent` — resize-drag and
nothing else. The reorder machinery (`EndDragging`, `ClearMarkers`) lives in
`wxHeaderCtrl`, which the generic list control does not use. So today you cannot
drag a column to reorder it, and the program cannot set an order either.

## 3. Why wiring it anyway would be worse than leaving it

`SaveColumnConfiguration` pre-seeds the array before asking the control:

```c
for (int i = 0; i < iColCount; i++)
    iColOrder [i] = i;              /* "in case our SendMessage doesn't work" */
ctlList.SendMessage (LVM_GETCOLUMNORDERARRAY, iColCount, (DWORD_PTR) iColOrder);
```

So the save path already *silently succeeds* with identity order. Adding the
message mapping would add code that reports success, writes identity, and
changes nothing — while making the backlog read FIXED. A no-op that looks like a
fix is worse than an acknowledged gap.

**Known live consequence, independent of the above.** Since `b16ea8c` revived
`CGenPropertyPage::OnDestroy`, `SaveColumnConfiguration` runs on every close of
World Properties and writes identity order into the `control` table. Harmless on
a Mac-only prefs database. A prefs database **carried over from Windows** has
its saved column order overwritten with identity. Cheap mitigation if this ever
matters: skip the Order write off Windows (~5 lines).

## 4. What a real fix would require

Swap the bridge's `SysListView32` mapping (`platform/wx_dialog_bridge.cpp:305`)
from `wxListCtrl` to `wxDataViewListCtrl`, whose `wxDataViewColumn::SetReorderable`
is real — on macOS `wxDataViewCtrl` is genuinely native, backed by
`NSOutlineView`, and that call forwards to the underlying `NSTableColumn`. (A
`wxHeaderCtrl` with `wxHD_ALLOW_REORDER` is the other route.)

The API needed all exists on macOS: `SetItemData`/`GetItemData` (`wxUIntPtr`,
so `LPARAM` maps cleanly), `EnsureVisible`, `HitTest`, `GetTopItem`,
`DoSet/GetCurrentItem`, `IsSortKey`/`SetSortOrder`/`UnsetAsSortKey`. The
migration is achievable. These are the costs.

## 5. The costs

### 5.1 Blast radius: 15 controls, 15 dialogs, at once

`SysListView32` is a **template mapping**, not a per-dialog choice. Every list in
the recovered resources changes control class together:

`IDD_PREFS_P6/P7/P8/P16/P18` (entity managers + macros), `IDD_PLUGINS`,
`IDD_PLUGINWIZARDPAGE3/4/5/6`, `IDD_CHAT_LIST`, `IDD_COLOUR_PICKER`,
`IDD_FUNCTION_LIST`, `IDD_LUA_CHOOSE_LIST`, `IDD_LUA_CHOOSE_LIST_MULTI`.

Two of those want column reordering. Thirteen inherit the risk.

### 5.2 Sorting changes semantics, not just spelling

The bridge currently forwards MFC's contract directly
(`platform/wx_mfc_impl.cpp` `hook_listctrl_sort_items`):

```cpp
lc->SortItems(ListSortTrampoline, (wxIntPtr) &ctx);
```

`CListCtrl::SortItems` is a **one-shot** sort with an arbitrary comparator that
receives the two rows' `LPARAM`s. `wxDataViewCtrl` has no equivalent. Sorting is
a property of the *model* — override `wxDataViewModel::Compare(item1, item2,
column, ascending)` — and it is **persistent**: the control re-sorts itself on
every insert and every value change.

That is a behaviour change. `CGenPropertyPage` sorts only on header click, with
a stateful comparator (`t_gen_sort_param` carries the object map, sort column,
reverse flag and a per-page compare function, constructed at click time,
`genpropertypage.cpp:817`). Under a self-sorting model, rows reorder mid-edit.
The entity pages' sort behaviour would have to be re-implemented rather than
bridged.

Second-order: on macOS a sortable `wxDataViewColumn` sorts **natively** on header
click. That would have to be suppressed to preserve the canonical three-state
cycle (same column toggles reverse, different column resets forward), or macOS
sorts differently from Windows.

### 5.3 Row indices stop being stable

Every canonical call site works in integer row indices — `GetNextItem`,
`SetItemState(i, …)`, `DeleteItem(i)`, `GetItemData(i)`; 7 `GetItemData` and 5
`SetItemState` calls in `genpropertypage.cpp` alone. `wxDataViewListCtrl`
identifies rows by `wxDataViewItem`, with `RowToItem`/`ItemToRow` as the bridge.

That conversion is mechanical. What is not: with a self-sorting model an index
captured before an insert can point at a *different row* afterwards. MFC code
assumes indices are stable between explicit sorts. This is the kind of bug that
survives a smoke test and appears when a user with 200 triggers deletes one.

### 5.4 Capability lost on the target control

`wx/dataview.h:795-803` is explicit that `SetHeaderAttr` and
`SetAlternateRowColour` are **generic-only**; the native macOS implementation
returns `false` for both. Any list styling wanted later is off the table on the
control we would be migrating *to*.

### 5.5 Sixteen hooks to re-implement

The whole bridged `CListCtrl` surface would need rebuilding against a different
model: `insert_column`, `insert_item`, `set_item_text`, `delete_item`,
`delete_all_items`, `get_item_count`, `set/get_item_data`, `set/get_item_state`,
`get_next_item`, `get_selected_count`, `ensure_visible`, `get/set_column_width`,
`sort_items`. Not conceptually hard; sixteen chances to regress dialogs that
work today.

Known-unknown while we are here: `genpropertypage.cpp` calls `RedrawItems` five
times and it is **not** among the bridged hooks, i.e. already inert. A migration
is when we would find out whether that matters.

### 5.6 Testing cost — the one that keeps being paid

None of this is reachable from `ctest`. The list controls live behind the dialog
bridge, which needs a running wx app. All fifteen dialogs would need manual GUI
verification — populate, sort, select, multi-select, delete, double-click,
reopen, confirm persistence — and re-verification on every later change in the
area. The migration is a few days; the permanent tax is fifteen dialogs' worth of
behaviour resting on a control with no automated coverage.

## 6. A latent trap to fix first, whoever does this

`platform/wx_mfc_impl.cpp` `bind_list_notifications` feeds a **visual** column
index into a field the canonical code reads as a **logical** one:

```cpp
nm.iSubItem = e.GetColumn();      // wxListEvent: VISUAL index
```

`CGenPropertyPage::OnColumnclickItemList` (`genpropertypage.cpp:808`) stores that
as `m_last_col` — the logical sort column — which is persisted as
`"<list> Sort Sequence"`. The two coincide today **only because the order is
always identity**. Make reordering work without fixing this and clicking a moved
header sorts by the wrong field.

This is dormant and harmless right now, and worth fixing on its own so it is not
a trap for a later reader.

## 7. When to revisit

Not for item 34. The trigger should be wanting `wxDataViewCtrl` **for its own
sake**, at which point column order arrives free:

* **A real tree control for the entity pages.** `genpropertypage.cpp:1534` hard-sets
  `m_bWantTreeControl = false` off Windows with the comment "there is no native
  tree-view alternative yet". `wxDataViewCtrl` *is* that alternative, and the
  canonical pages already have a full tree code path behind that flag.
* **Per-row styling** in the entity lists (colouring disabled triggers, say).
* **wxWidgets implementing `wxGenericListCtrl::SetColumnsOrder`.** Re-check the
  disassembly in §2 after a wx upgrade; if it stops being `mov w0,#0; ret`, this
  whole document collapses into a small `hook_send_message` case plus the §6 fix.
