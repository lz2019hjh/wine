/*
 * HID descriptor parsing
 *
 * Copyright (C) 2015 Aric Stewart
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 */

#include <stdarg.h>
#include <stdlib.h>
#include <stdio.h>
#include "hid.h"

#include "wine/debug.h"
#include "wine/list.h"

WINE_DEFAULT_DEBUG_CHANNEL(hid);

/* Flags that are defined in the document
   "Device Class Definition for Human Interface Devices" */
enum {
    INPUT_DATA_CONST = 0x01, /* Data (0)             | Constant (1)       */
    INPUT_ARRAY_VAR = 0x02,  /* Array (0)            | Variable (1)       */
    INPUT_ABS_REL = 0x04,    /* Absolute (0)         | Relative (1)       */
    INPUT_WRAP = 0x08,       /* No Wrap (0)          | Wrap (1)           */
    INPUT_LINEAR = 0x10,     /* Linear (0)           | Non Linear (1)     */
    INPUT_PREFSTATE = 0x20,  /* Preferred State (0)  | No Preferred (1)   */
    INPUT_NULL = 0x40,       /* No Null position (0) | Null state(1)      */
    INPUT_VOLATILE = 0x80,   /* Non Volatile (0)     | Volatile (1)       */
    INPUT_BITFIELD = 0x100   /* Bit Field (0)        | Buffered Bytes (1) */
};

enum {
    TAG_TYPE_MAIN = 0x0,
    TAG_TYPE_GLOBAL,
    TAG_TYPE_LOCAL,
    TAG_TYPE_RESERVED,
};

enum {
    TAG_MAIN_INPUT = 0x08,
    TAG_MAIN_OUTPUT = 0x09,
    TAG_MAIN_FEATURE = 0x0B,
    TAG_MAIN_COLLECTION = 0x0A,
    TAG_MAIN_END_COLLECTION = 0x0C
};

enum {
    TAG_GLOBAL_USAGE_PAGE = 0x0,
    TAG_GLOBAL_LOGICAL_MINIMUM,
    TAG_GLOBAL_LOGICAL_MAXIMUM,
    TAG_GLOBAL_PHYSICAL_MINIMUM,
    TAG_GLOBAL_PHYSICAL_MAXIMUM,
    TAG_GLOBAL_UNIT_EXPONENT,
    TAG_GLOBAL_UNIT,
    TAG_GLOBAL_REPORT_SIZE,
    TAG_GLOBAL_REPORT_ID,
    TAG_GLOBAL_REPORT_COUNT,
    TAG_GLOBAL_PUSH,
    TAG_GLOBAL_POP
};

enum {
    TAG_LOCAL_USAGE = 0x0,
    TAG_LOCAL_USAGE_MINIMUM,
    TAG_LOCAL_USAGE_MAXIMUM,
    TAG_LOCAL_DESIGNATOR_INDEX,
    TAG_LOCAL_DESIGNATOR_MINIMUM,
    TAG_LOCAL_DESIGNATOR_MAXIMUM,
    TAG_LOCAL_STRING_INDEX,
    TAG_LOCAL_STRING_MINIMUM,
    TAG_LOCAL_STRING_MAXIMUM,
    TAG_LOCAL_DELIMITER
};


static const char* const feature_string[] =
    { "Input", "Output", "Feature" };

struct feature {
    struct list entry;
    HIDP_VALUE_CAPS caps;

    HIDP_REPORT_TYPE type;
    BOOLEAN isData;
    BOOLEAN isArray;
    BOOLEAN IsAbsolute;
    BOOLEAN Wrap;
    BOOLEAN Linear;
    BOOLEAN prefState;
    BOOLEAN HasNull;
    BOOLEAN Volatile;
    BOOLEAN BitField;

    unsigned int index;
    struct collection *collection;
};

static const char* const collection_string[] = {
    "Physical",
    "Application",
    "Logical",
    "Report",
    "Named Array",
    "Usage Switch",
    "Usage Modifier",
};

struct collection {
    struct list entry;
    HIDP_VALUE_CAPS caps;
    unsigned int index;
    unsigned int type;
    struct collection *parent;
    struct list features;
    struct list collections;
};

struct caps_stack {
    struct list entry;
    HIDP_VALUE_CAPS caps;
};

static inline const char *debugstr_hidp_value_caps( HIDP_VALUE_CAPS *caps )
{
    if (!caps) return "(null)";
    return wine_dbg_sprintf( "RId %d, Usg %02x:%02x-%02x Dat %02x-%02x (%d), Str %d-%d (%d), Des %d-%d (%d), "
                             "Bits %02x, Als %d, Abs %d, Nul %d, LCol %d LUsg %02x:%02x, BitSz %d, RCnt %d, "
                             "Unit %x E%+d, Log %+d-%+d, Phy %+d-%+d",
                             caps->ReportID, caps->UsagePage, caps->Range.UsageMin, caps->Range.UsageMax, caps->Range.DataIndexMin, caps->Range.DataIndexMax, caps->IsRange,
                             caps->Range.StringMin, caps->Range.StringMax, caps->IsStringRange, caps->Range.DesignatorMin, caps->Range.DesignatorMax, caps->IsDesignatorRange,
                             caps->BitField, caps->IsAlias, caps->IsAbsolute, caps->HasNull, caps->LinkCollection, caps->LinkUsagePage, caps->LinkUsage, caps->BitSize, caps->ReportCount,
                             caps->Units, caps->UnitsExp, caps->LogicalMin, caps->LogicalMax, caps->PhysicalMin, caps->PhysicalMax );
}

static void debug_feature(struct feature *feature)
{
    if (!feature)
        return;
    TRACE("[Feature type %s [%i]; %s; %s; %s; %s; %s; %s; %s; %s; %s]\n",
    feature_string[feature->type],
    feature->index,
    (feature->isData)?"Data":"Const",
    (feature->isArray)?"Array":"Var",
    (feature->IsAbsolute)?"Abs":"Rel",
    (feature->Wrap)?"Wrap":"NoWrap",
    (feature->Linear)?"Linear":"NonLinear",
    (feature->prefState)?"PrefStat":"NoPrefState",
    (feature->HasNull)?"HasNull":"NoNull",
    (feature->Volatile)?"Volatile":"NonVolatile",
    (feature->BitField)?"BitField":"Buffered");

    TRACE("Feature %s\n", debugstr_hidp_value_caps(&feature->caps));
}

static void debug_collection(struct collection *collection)
{
    struct feature *fentry;
    struct collection *centry;
    if (TRACE_ON(hid))
    {
        TRACE("START Collection %i <<< %s, parent: %p,  %i features,  %i collections\n",
                collection->index, collection_string[collection->type], collection->parent,
                list_count(&collection->features), list_count(&collection->collections));
        TRACE("Collection %s\n", debugstr_hidp_value_caps(&collection->caps));
        LIST_FOR_EACH_ENTRY(fentry, &collection->features, struct feature, entry)
            debug_feature(fentry);
        LIST_FOR_EACH_ENTRY(centry, &collection->collections, struct collection, entry)
            debug_collection(centry);
        TRACE(">>> END Collection %i\n", collection->index);
    }
}

static void debug_print_report(const char* type, WINE_HIDP_PREPARSED_DATA *data,
        WINE_HID_REPORT *report)
{
    WINE_HID_ELEMENT *elems = HID_ELEMS(data);
    unsigned int i;
    TRACE("START Report %i <<< %s report : bitSize: %i elementCount: %i\n",
        report->reportID,
        type,
        report->bitSize,
        report->elementCount);
    for (i = 0; i < report->elementCount; i++)
    {
        WINE_HID_ELEMENT *elem = elems + report->elementIdx + i;
        TRACE("%s: %s, StartBit %d, BitCount %d\n", type, debugstr_hidp_value_caps(&elem->caps), elem->valueStartBit, elem->bitCount);
    }
    TRACE(">>> END Report %i\n",report->reportID);
}

static void debug_print_preparsed(WINE_HIDP_PREPARSED_DATA *data)
{
    unsigned int i, end;
    if (TRACE_ON(hid))
    {
        TRACE("START PREPARSED Data <<< dwSize: %i Usage: %i, UsagePage: %i, "
                "InputReportByteLength: %i, tOutputReportByteLength: %i, "
                "FeatureReportByteLength: %i, NumberLinkCollectionNodes: %i, "
                "NumberInputButtonCaps: %i, NumberInputValueCaps: %i, "
                "NumberInputDataIndices: %i, NumberOutputButtonCaps: %i, "
                "NumberOutputValueCaps: %i, NumberOutputDataIndices: %i, "
                "NumberFeatureButtonCaps: %i, NumberFeatureValueCaps: %i, "
                "NumberFeatureDataIndices: %i, reportCount[HidP_Input]: %i, "
                "reportCount[HidP_Output]: %i, reportCount[HidP_Feature]: %i, "
                "elementOffset: %i\n",
        data->dwSize,
        data->caps.Usage,
        data->caps.UsagePage,
        data->caps.InputReportByteLength,
        data->caps.OutputReportByteLength,
        data->caps.FeatureReportByteLength,
        data->caps.NumberLinkCollectionNodes,
        data->caps.NumberInputButtonCaps,
        data->caps.NumberInputValueCaps,
        data->caps.NumberInputDataIndices,
        data->caps.NumberOutputButtonCaps,
        data->caps.NumberOutputValueCaps,
        data->caps.NumberOutputDataIndices,
        data->caps.NumberFeatureButtonCaps,
        data->caps.NumberFeatureValueCaps,
        data->caps.NumberFeatureDataIndices,
        data->reportCount[HidP_Input],
        data->reportCount[HidP_Output],
        data->reportCount[HidP_Feature],
        data->elementOffset);

        end = data->reportCount[HidP_Input];
        for (i = 0; i < end; i++)
        {
            debug_print_report("INPUT", data, &data->reports[i]);
        }
        end += data->reportCount[HidP_Output];
        for (; i < end; i++)
        {
            debug_print_report("OUTPUT", data, &data->reports[i]);
        }
        end += data->reportCount[HidP_Feature];
        for (; i < end; i++)
        {
            debug_print_report("FEATURE", data, &data->reports[i]);
        }
        TRACE(">>> END Preparsed Data\n");
    }
}

static int getValue(int bsize, int source, BOOL allow_negative)
{
    int mask = 0xff;
    int negative = 0x80;
    int outofrange = 0x100;
    int value;
    unsigned int i;

    if (bsize == 4)
        return source;

    for (i = 1; i < bsize; i++)
    {
        mask = (mask<<8) + 0xff;
        negative = (negative<<8);
        outofrange = (outofrange<<8);
    }
    value = (source&mask);
    if (allow_negative && value&negative)
        value = -1 * (outofrange - value);
    return value;
}

static void parse_io_feature(unsigned int bSize, int itemVal, int bTag,
                             unsigned int *feature_index,
                             struct feature *feature)
{
    if (bSize == 0)
    {
        return;
    }
    else
    {
        feature->isData = ((itemVal & INPUT_DATA_CONST) == 0);
        feature->isArray =  ((itemVal & INPUT_ARRAY_VAR) == 0);
        feature->IsAbsolute = ((itemVal & INPUT_ABS_REL) == 0);
        feature->Wrap = ((itemVal & INPUT_WRAP) != 0);
        feature->Linear = ((itemVal & INPUT_LINEAR) == 0);
        feature->prefState = ((itemVal & INPUT_PREFSTATE) == 0);
        feature->HasNull = ((itemVal & INPUT_NULL) != 0);

        if (bTag != TAG_MAIN_INPUT)
        {
            feature->Volatile = ((itemVal & INPUT_VOLATILE) != 0);
        }
        if (bSize > 1)
        {
            feature->BitField = ((itemVal & INPUT_BITFIELD) == 0);
        }
        feature->index = *feature_index;
        *feature_index = *feature_index + 1;
    }
}

static void parse_collection(unsigned int bSize, int itemVal,
                             struct collection *collection)
{
    if (bSize)
    {
        collection->type = itemVal;

        if (itemVal >= 0x07 && itemVal <= 0x7F) {
            ERR(" (Reserved 0x%x )\n", itemVal);
        }
        else if (itemVal >= 0x80 && itemVal <= 0xFF) {
            ERR(" (Vendor Defined 0x%x )\n", itemVal);
        }
    }
}

static void new_caps(HIDP_VALUE_CAPS *caps)
{
    caps->IsRange = 0;
    caps->IsStringRange = 0;
    caps->IsDesignatorRange = 0;
    caps->NotRange.Usage = 0;
}

static int parse_descriptor(BYTE *descriptor, unsigned int index, unsigned int length,
                            unsigned int *feature_index, unsigned int *collection_index,
                            struct collection *collection, HIDP_VALUE_CAPS *caps,
                            struct list *stack)
{
    int usages_top = 0;
    USAGE usages[256];
    int i;

    for (i = index; i < length;)
    {
        BYTE b0 = descriptor[i++];
        int bSize = b0 & 0x03;
        int bType = (b0 >> 2) & 0x03;
        int bTag = (b0 >> 4) & 0x0F;

        bSize = (bSize == 3) ? 4 : bSize;
        if (bType == TAG_TYPE_RESERVED && bTag == 0x0F && bSize == 2 &&
            i + 2 < length)
        {
            /* Long data items: Should be unused */
            ERR("Long Data Item, should be unused\n");
            return -1;
        }
        else
        {
            int bSizeActual = 0;
            int itemVal = 0;
            unsigned int j;

            for (j = 0; j < bSize; j++)
            {
                if (i + j < length)
                {
                    itemVal += descriptor[i + j] << (8 * j);
                    bSizeActual++;
                }
            }
            TRACE(" 0x%x[%i], type %i , tag %i, size %i, val %i\n",b0,i-1,bType, bTag, bSize, itemVal );

            if (bType == TAG_TYPE_MAIN)
            {
                struct feature *feature;
                switch(bTag)
                {
                    case TAG_MAIN_INPUT:
                    case TAG_MAIN_OUTPUT:
                    case TAG_MAIN_FEATURE:
                        for (j = 0; j < caps->ReportCount; j++)
                        {
                            if (!(feature = calloc(1, sizeof(*feature)))) return -1;
                            list_add_tail(&collection->features, &feature->entry);
                            if (bTag == TAG_MAIN_INPUT)
                                feature->type = HidP_Input;
                            else if (bTag == TAG_MAIN_OUTPUT)
                                feature->type = HidP_Output;
                            else
                                feature->type = HidP_Feature;
                            parse_io_feature(bSize, itemVal, bTag, feature_index, feature);
                            if (j < usages_top)
                                caps->NotRange.Usage = usages[j];
                            feature->caps = *caps;
                            feature->caps.ReportCount = 1;
                            feature->collection = collection;
                            if (j+1 >= usages_top)
                            {
                                feature->caps.ReportCount += caps->ReportCount - (j + 1);
                                break;
                            }
                        }
                        usages_top = 0;
                        new_caps(caps);
                        break;
                    case TAG_MAIN_COLLECTION:
                    {
                        struct collection *subcollection;
                        if (!(subcollection = calloc(1, sizeof(struct collection)))) return -1;
                        list_add_tail(&collection->collections, &subcollection->entry);
                        subcollection->parent = collection;
                        /* Only set our collection once...
                           We do not properly handle composite devices yet. */
                        if (usages_top)
                        {
                            caps->NotRange.Usage = usages[usages_top-1];
                            usages_top = 0;
                        }
                        if (*collection_index == 0)
                            collection->caps = *caps;
                        subcollection->caps = *caps;
                        subcollection->index = *collection_index;
                        *collection_index = *collection_index + 1;
                        list_init(&subcollection->features);
                        list_init(&subcollection->collections);
                        new_caps(caps);

                        parse_collection(bSize, itemVal, subcollection);

                        if ((i = parse_descriptor(descriptor, i+1, length, feature_index, collection_index, subcollection, caps, stack)) < 0) return i;
                        continue;
                    }
                    case TAG_MAIN_END_COLLECTION:
                        return i;
                    default:
                        ERR("Unknown (bTag: 0x%x, bType: 0x%x)\n", bTag, bType);
                        return -1;
                }
            }
            else if (bType == TAG_TYPE_GLOBAL)
            {
                switch(bTag)
                {
                    case TAG_GLOBAL_USAGE_PAGE:
                        caps->UsagePage = getValue(bSize, itemVal, FALSE);
                        break;
                    case TAG_GLOBAL_LOGICAL_MINIMUM:
                        caps->LogicalMin = getValue(bSize, itemVal, TRUE);
                        break;
                    case TAG_GLOBAL_LOGICAL_MAXIMUM:
                        caps->LogicalMax = getValue(bSize, itemVal, TRUE);
                        break;
                    case TAG_GLOBAL_PHYSICAL_MINIMUM:
                        caps->PhysicalMin = getValue(bSize, itemVal, TRUE);
                        break;
                    case TAG_GLOBAL_PHYSICAL_MAXIMUM:
                        caps->PhysicalMax = getValue(bSize, itemVal, TRUE);
                        break;
                    case TAG_GLOBAL_UNIT_EXPONENT:
                        caps->UnitsExp = getValue(bSize, itemVal, TRUE);
                        break;
                    case TAG_GLOBAL_UNIT:
                        caps->Units = getValue(bSize, itemVal, TRUE);
                        break;
                    case TAG_GLOBAL_REPORT_SIZE:
                        caps->BitSize = getValue(bSize, itemVal, FALSE);
                        break;
                    case TAG_GLOBAL_REPORT_ID:
                        caps->ReportID = getValue(bSize, itemVal, FALSE);
                        break;
                    case TAG_GLOBAL_REPORT_COUNT:
                        caps->ReportCount = getValue(bSize, itemVal, FALSE);
                        break;
                    case TAG_GLOBAL_PUSH:
                    {
                        struct caps_stack *saved;
                        if (!(saved = malloc(sizeof(*saved)))) return -1;
                        saved->caps = *caps;
                        TRACE("Push\n");
                        list_add_tail(stack, &saved->entry);
                        break;
                    }
                    case TAG_GLOBAL_POP:
                    {
                        struct list *tail;
                        struct caps_stack *saved;
                        TRACE("Pop\n");
                        tail = list_tail(stack);
                        if (tail)
                        {
                            saved = LIST_ENTRY(tail, struct caps_stack, entry);
                            *caps = saved->caps;
                            list_remove(tail);
                            free(saved);
                        }
                        else
                        {
                            ERR("Pop but no stack!\n");
                            return -1;
                        }
                        break;
                    }
                    default:
                        ERR("Unknown (bTag: 0x%x, bType: 0x%x)\n", bTag, bType);
                        return -1;
                }
            }
            else if (bType == TAG_TYPE_LOCAL)
            {
                switch(bTag)
                {
                    case TAG_LOCAL_USAGE:
                        if (usages_top == sizeof(usages))
                        {
                            ERR("More than 256 individual usages defined\n");
                            return -1;
                        }
                        else
                        {
                            usages[usages_top++] = getValue(bSize, itemVal, FALSE);
                            caps->IsRange = FALSE;
                        }
                        break;
                    case TAG_LOCAL_USAGE_MINIMUM:
                        caps->Range.UsageMin = getValue(bSize, itemVal, FALSE);
                        caps->IsRange = TRUE;
                        break;
                    case TAG_LOCAL_USAGE_MAXIMUM:
                        caps->Range.UsageMax = getValue(bSize, itemVal, FALSE);
                        caps->IsRange = TRUE;
                        break;
                    case TAG_LOCAL_DESIGNATOR_INDEX:
                        caps->NotRange.DesignatorIndex = getValue(bSize, itemVal, FALSE);
                        caps->IsDesignatorRange = FALSE;
                        break;
                    case TAG_LOCAL_DESIGNATOR_MINIMUM:
                        caps->Range.DesignatorMin = getValue(bSize, itemVal, FALSE);
                        caps->IsDesignatorRange = TRUE;
                        break;
                    case TAG_LOCAL_DESIGNATOR_MAXIMUM:
                        caps->Range.DesignatorMax = getValue(bSize, itemVal, FALSE);
                        caps->IsDesignatorRange = TRUE;
                        break;
                    case TAG_LOCAL_STRING_INDEX:
                        caps->NotRange.StringIndex = getValue(bSize, itemVal, FALSE);
                        caps->IsStringRange = FALSE;
                        break;
                    case TAG_LOCAL_STRING_MINIMUM:
                        caps->Range.StringMin = getValue(bSize, itemVal, FALSE);
                        caps->IsStringRange = TRUE;
                        break;
                    case TAG_LOCAL_STRING_MAXIMUM:
                        caps->Range.StringMax = getValue(bSize, itemVal, FALSE);
                        caps->IsStringRange = TRUE;
                        break;
                    case TAG_LOCAL_DELIMITER:
                        FIXME("delimiter %d not implemented!\n", itemVal);
                        return -1;
                    default:
                        ERR("Unknown (bTag: 0x%x, bType: 0x%x)\n", bTag, bType);
                        return -1;
                }
            }
            else
            {
                ERR("Unknown (bTag: 0x%x, bType: 0x%x)\n", bTag, bType);
                return -1;
            }

            i += bSize;
        }
    }
    return i;
}

static void build_elements(WINE_HID_REPORT *wine_report, WINE_HID_ELEMENT *elems,
        struct feature* feature, USHORT *data_index)
{
    WINE_HID_ELEMENT *wine_element = elems + wine_report->elementIdx + wine_report->elementCount;
    ULONG index_count;

    if (!feature->isData)
    {
        wine_report->bitSize += feature->caps.BitSize * feature->caps.ReportCount;
        return;
    }

    wine_element->valueStartBit = wine_report->bitSize;

    wine_element->bitCount = (feature->caps.BitSize * feature->caps.ReportCount);
    wine_report->bitSize += wine_element->bitCount;

    wine_element->caps = feature->caps;
    wine_element->caps.BitField = feature->BitField;
    wine_element->caps.LinkCollection = feature->collection->index;
    wine_element->caps.LinkUsage = feature->collection->caps.NotRange.Usage;
    wine_element->caps.LinkUsagePage = feature->collection->caps.UsagePage;
    wine_element->caps.IsAbsolute = feature->IsAbsolute;
    wine_element->caps.HasNull = feature->HasNull;

    if (wine_element->caps.IsRange)
    {
        if (wine_element->caps.BitSize == 1) index_count = wine_element->bitCount - 1;
        else index_count = wine_element->caps.Range.UsageMax - wine_element->caps.Range.UsageMin;
        wine_element->caps.Range.DataIndexMin = *data_index;
        wine_element->caps.Range.DataIndexMax = *data_index + index_count;
        *data_index = *data_index + index_count + 1;
    }
    else
    {
        wine_element->caps.NotRange.DataIndex = *data_index;
        wine_element->caps.NotRange.Reserved4 = *data_index;
        *data_index = *data_index + 1;
    }

    wine_report->elementCount++;
}

static void count_elements(struct feature* feature, USHORT *buttons, USHORT *values)
{
    if (!feature->isData)
        return;

    if (feature->caps.BitSize == 1)
        (*buttons)++;
    else
        (*values)++;
}

struct preparse_ctx
{
    int report_count[3];
    int elem_count;
    int report_elem_count[3][256];

    int elem_alloc;
    BOOL report_created[3][256];
};

static void create_preparse_ctx(const struct collection *base, struct preparse_ctx *ctx)
{
    struct feature *f;
    struct collection *c;

    LIST_FOR_EACH_ENTRY(f, &base->features, struct feature, entry)
    {
        ctx->elem_count++;
        ctx->report_elem_count[f->type][f->caps.ReportID]++;
        if (ctx->report_elem_count[f->type][f->caps.ReportID] != 1)
            continue;
        ctx->report_count[f->type]++;
    }

    LIST_FOR_EACH_ENTRY(c, &base->collections, struct collection, entry)
        create_preparse_ctx(c, ctx);
}

static void preparse_collection(const struct collection *root, const struct collection *base,
        WINE_HIDP_PREPARSED_DATA *data, struct preparse_ctx *ctx)
{
    WINE_HID_ELEMENT *elem = HID_ELEMS(data);
    WINE_HID_LINK_COLLECTION_NODE *nodes = HID_NODES(data);
    struct feature *f;
    struct collection *c;
    struct list *entry;

    LIST_FOR_EACH_ENTRY(f, &base->features, struct feature, entry)
    {
        WINE_HID_REPORT *report;

        if (!ctx->report_created[f->type][f->caps.ReportID])
        {
            ctx->report_created[f->type][f->caps.ReportID] = TRUE;
            data->reportIdx[f->type][f->caps.ReportID] = data->reportCount[f->type]++;
            if (f->type > 0) data->reportIdx[f->type][f->caps.ReportID] += ctx->report_count[0];
            if (f->type > 1) data->reportIdx[f->type][f->caps.ReportID] += ctx->report_count[1];

            report = &data->reports[data->reportIdx[f->type][f->caps.ReportID]];
            report->reportID = f->caps.ReportID;
            /* Room for the reportID */
            report->bitSize = 8;
            report->elementIdx = ctx->elem_alloc;
            ctx->elem_alloc += ctx->report_elem_count[f->type][f->caps.ReportID];
        }

        report = &data->reports[data->reportIdx[f->type][f->caps.ReportID]];
        switch (f->type)
        {
            case HidP_Input:
                build_elements(report, elem, f, &data->caps.NumberInputDataIndices);
                count_elements(f, &data->caps.NumberInputButtonCaps, &data->caps.NumberInputValueCaps);
                data->caps.InputReportByteLength =
                    max(data->caps.InputReportByteLength, (report->bitSize + 7) / 8);
                break;
            case HidP_Output:
                build_elements(report, elem, f, &data->caps.NumberOutputDataIndices);
                count_elements(f, &data->caps.NumberOutputButtonCaps, &data->caps.NumberOutputValueCaps);
                data->caps.OutputReportByteLength =
                    max(data->caps.OutputReportByteLength, (report->bitSize + 7) / 8);
                break;
            case HidP_Feature:
                build_elements(report, elem, f, &data->caps.NumberFeatureDataIndices);
                count_elements(f, &data->caps.NumberFeatureButtonCaps, &data->caps.NumberFeatureValueCaps);
                data->caps.FeatureReportByteLength =
                    max(data->caps.FeatureReportByteLength, (report->bitSize + 7) / 8);
                break;
        }
    }

    if (root != base)
    {
        nodes[base->index].LinkUsagePage = base->caps.UsagePage;
        nodes[base->index].LinkUsage = base->caps.NotRange.Usage;
        nodes[base->index].Parent = base->parent == root ? 0 : base->parent->index;
        nodes[base->index].CollectionType = base->type;
        nodes[base->index].IsAlias = 0;

        if ((entry = list_head(&base->collections)))
            nodes[base->index].FirstChild = LIST_ENTRY(entry, struct collection, entry)->index;
    }

    LIST_FOR_EACH_ENTRY(c, &base->collections, struct collection, entry)
    {
        preparse_collection(root, c, data, ctx);

        if ((entry = list_next(&base->collections, &c->entry)))
            nodes[c->index].NextSibling = LIST_ENTRY(entry, struct collection, entry)->index;
        if (root != base) nodes[base->index].NumberOfChildren++;
    }
}

static WINE_HIDP_PREPARSED_DATA* build_PreparseData(struct collection *base_collection, unsigned int node_count)
{
    WINE_HIDP_PREPARSED_DATA *data;
    unsigned int report_count;
    unsigned int size;

    struct preparse_ctx ctx;
    unsigned int element_off;
    unsigned int nodes_offset;

    memset(&ctx, 0, sizeof(ctx));
    create_preparse_ctx(base_collection, &ctx);

    report_count = ctx.report_count[HidP_Input] + ctx.report_count[HidP_Output]
        + ctx.report_count[HidP_Feature];
    element_off = FIELD_OFFSET(WINE_HIDP_PREPARSED_DATA, reports[report_count]);
    size = element_off + (ctx.elem_count * sizeof(WINE_HID_ELEMENT));

    nodes_offset = size;
    size += node_count * sizeof(WINE_HID_LINK_COLLECTION_NODE);

    if (!(data = calloc(1, size))) return NULL;
    data->magic = HID_MAGIC;
    data->dwSize = size;
    data->caps.Usage = base_collection->caps.NotRange.Usage;
    data->caps.UsagePage = base_collection->caps.UsagePage;
    data->caps.NumberLinkCollectionNodes = node_count;
    data->elementOffset = element_off;
    data->nodesOffset = nodes_offset;

    preparse_collection(base_collection, base_collection, data, &ctx);
    return data;
}

static void free_collection(struct collection *collection)
{
    struct feature *fentry, *fnext;
    struct collection *centry, *cnext;
    LIST_FOR_EACH_ENTRY_SAFE(centry, cnext, &collection->collections, struct collection, entry)
    {
        list_remove(&centry->entry);
        free_collection(centry);
    }
    LIST_FOR_EACH_ENTRY_SAFE(fentry, fnext, &collection->features, struct feature, entry)
    {
        list_remove(&fentry->entry);
        free(fentry);
    }
    free(collection);
}

WINE_HIDP_PREPARSED_DATA* ParseDescriptor(BYTE *descriptor, unsigned int length)
{
    WINE_HIDP_PREPARSED_DATA *data = NULL;
    struct collection *base;
    HIDP_VALUE_CAPS caps;
    int i;

    struct list caps_stack;

    unsigned int feature_count = 0;
    unsigned int cidx;

    if (TRACE_ON(hid))
    {
        TRACE("descriptor %p, length %u:\n", descriptor, length);
        for (i = 0; i < length;)
        {
            TRACE("%08x ", i);
            do { TRACE(" %02x", descriptor[i]); } while (++i % 16 && i < length);
            TRACE("\n");
        }
    }

    list_init(&caps_stack);

    if (!(base = calloc(1, sizeof(*base)))) return NULL;
    base->index = 1;
    list_init(&base->features);
    list_init(&base->collections);
    memset(&caps, 0, sizeof(caps));

    cidx = 0;
    if (parse_descriptor(descriptor, 0, length, &feature_count, &cidx, base, &caps, &caps_stack) < 0)
    {
        free_collection(base);
        return NULL;
    }

    debug_collection(base);

    if (!list_empty(&caps_stack))
    {
        struct caps_stack *entry, *cursor;
        ERR("%i unpopped device caps on the stack\n", list_count(&caps_stack));
        LIST_FOR_EACH_ENTRY_SAFE(entry, cursor, &caps_stack, struct caps_stack, entry)
        {
            list_remove(&entry->entry);
            free(entry);
        }
    }

    if ((data = build_PreparseData(base, cidx)))
        debug_print_preparsed(data);
    free_collection(base);

    return data;
}
