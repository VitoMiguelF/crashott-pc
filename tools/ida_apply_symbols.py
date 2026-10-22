# Apply the names recovered by tools/rtti_scan.py to a Crash of the Titans XEX opened in IDA
# (IDAPython, IDA 7.x-9.x; File > Script file...). Pick private/symbols/functions.csv; rtti.json
# next to it adds vtable / RTTI labels.
#
# IDA names cannot contain spaces or most punctuation: SN_NOCHECK makes IDA replace them with '_',
# and the original name goes into the function comment. Names you set by hand are kept.
import csv
import json
import os

import ida_bytes
import ida_funcs
import ida_kernwin
import ida_name
import idc


def set_name(ea, name):
    flags = ida_name.SN_NOCHECK | ida_name.SN_NOWARN | ida_name.SN_FORCE
    return ida_name.set_name(ea, name, flags)


def main():
    path = ida_kernwin.ask_file(False, '*.csv', 'functions.csv (tools/rtti_scan.py)')
    if not path:
        return
    named = kept = 0
    with open(path, newline='', encoding='utf-8') as fp:
        rows = list(csv.DictReader(fp))
    for row in rows:
        ea = int(row['address'], 16)
        if not ida_funcs.get_func(ea):
            ida_funcs.add_func(ea)
        func = ida_funcs.get_func(ea)
        # A user name without our comment was set by hand (or by the loader for imports, which
        # are renamed anyway): keep it.
        ours = func is not None and '[' in (ida_funcs.get_func_cmt(func, True) or '')
        if ida_bytes.has_user_name(ida_bytes.get_flags(ea)) and not ours and row['source'] != 'import':
            kept += 1
            continue
        set_name(ea, row['name'])
        comment = '%s [%s]%s' % (row['name'], row['source'], (' ' + row['note']) if row.get('note') else '')
        if func:
            ida_funcs.set_func_cmt(func, comment, True)
        else:
            idc.set_cmt(ea, comment, True)
        named += 1

    vtables = 0
    rtti_path = os.path.join(os.path.dirname(path), 'rtti.json')
    if os.path.exists(rtti_path):
        with open(rtti_path, encoding='utf-8') as fp:
            rtti = json.load(fp)
        for cls in rtti['classes']:
            set_name(int(cls['type_descriptor'], 16), cls['name'] + '::RTTI_Type_Descriptor')
            for vt in cls['vtables']:
                suffix = '' if not vt['subobject'] else '_for_' + vt['subobject']
                set_name(int(vt['address'], 16), cls['name'] + '::vftable' + suffix)
                set_name(int(vt['col'], 16), cls['name'] + '::RTTI_Complete_Object_Locator' + suffix)
                vtables += 1

    print('CrashRecomp symbols: %d functions named, %d kept (user names), %d vtables labelled' % (named, kept, vtables))


main()
