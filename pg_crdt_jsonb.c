#include "postgres.h"

#include "fmgr.h"
#include "executor/spi.h"
#include "libpq/pqformat.h"
#include "utils/array.h"
#include "utils/builtins.h"
#include "utils/jsonb.h"
#include "utils/timestamp.h"

PG_MODULE_MAGIC;

#define CRDTJSONB_EMPTY_ARRAY "[]"
#define CRDTJSONB_NO_INITIAL_VALUE ""

#define VECTOR_MAGNIFICATION_NUMBER 8

#define GET_UPDATESP(cj) (CustomVector *)(&cj->updates)
#define GET_VALUEP(cj) (Jsonb *)((char *)&cj->updates + VARSIZE(&cj->updates))

typedef struct
{
    int32 vl_len_;
    uint32 capacity;
    uint32 size;
    uint32 item_size;
    /* after these fields there will be array in memory, it will take up (capacity * item_size) bytes */
} CustomVector;

typedef struct
{
    int32 vl_len_;
    int32 update_counter;
    CustomVector updates;
    Jsonb value;
} CrdtJsonb;

Datum crdt_jsonb_in(PG_FUNCTION_ARGS);
Datum crdt_jsonb_out(PG_FUNCTION_ARGS);
Datum crdt_jsonb_recv(PG_FUNCTION_ARGS);
Datum crdt_jsonb_send(PG_FUNCTION_ARGS);
Datum crdt_jsonb_append(PG_FUNCTION_ARGS);

Datum get_jsonb(PG_FUNCTION_ARGS);
CrdtJsonb *merge(CrdtJsonb *cj_old, const Jsonb *appended_jsonb, const Timestamp update_time);
static Jsonb *get_last_jsonb(Jsonb *jsonb_array);
static int32 find_index_for_insert(const CrdtJsonb *cj, const Timestamp update_time);

static CustomVector *create_vector(const uint32 initial_capacity, const uint32 item_size);
static CustomVector *resize_vector(CustomVector *vector);
static void *get_vector_item(const CustomVector *vector, const uint32 index);
static void set_vector_item(CustomVector *vector, const uint32 index, const void* item);
static CustomVector *insert_vector_item(CustomVector *vector, const uint32 index, const void *item);


PG_FUNCTION_INFO_V1(crdt_jsonb_in);
PG_FUNCTION_INFO_V1(crdt_jsonb_out);
PG_FUNCTION_INFO_V1(crdt_jsonb_recv);
PG_FUNCTION_INFO_V1(crdt_jsonb_send);
PG_FUNCTION_INFO_V1(crdt_jsonb_append);
PG_FUNCTION_INFO_V1(get_jsonb);

static inline CrdtJsonb *
DatumGetCrdtJsonbP(Datum d)
{
    return (CrdtJsonb *) PG_DETOAST_DATUM(d);
}

static inline Datum
CrdtJsonbPGetDatum(const CrdtJsonb *p)
{
    return PointerGetDatum(p);
}

#define PG_GETARG_CRDTJSONB_P(x)	DatumGetCrdtJsonbP(PG_GETARG_DATUM(x))
#define PG_RETURN_CRDTJSONB_P(x)	PG_RETURN_POINTER(x)

Datum
crdt_jsonb_in(PG_FUNCTION_ARGS)
{
    CrdtJsonb *cj;
    Jsonb *append_value;
    CustomVector *updates;
    Jsonb *jsonb_array, *new_value;
    char *json_str, *jsonb_array_str;
    int cj_len, counter;
    Timestamp cur_time;

    Datum path, path_datum;
    text *path_text;

    json_str = PG_GETARG_CSTRING(0);

    jsonb_array_str = CRDTJSONB_EMPTY_ARRAY;
    jsonb_array = DatumGetJsonbP(DirectFunctionCall1(jsonb_in, CStringGetDatum(jsonb_array_str)));
    updates = create_vector(2, sizeof(Timestamp));

    if (strcmp(json_str, CRDTJSONB_NO_INITIAL_VALUE) != 0) 
    {
        append_value = DatumGetJsonbP(DirectFunctionCall1(jsonb_in, CStringGetDatum(json_str)));

        path_text = cstring_to_text("0");
        path_datum = PointerGetDatum(path_text);
        path = PointerGetDatum(construct_array(&path_datum, 1, TEXTOID, -1, false, 'i'));

        new_value = DatumGetJsonbP(DirectFunctionCall4Coll(
            jsonb_insert,
            InvalidOid,
            JsonbPGetDatum(jsonb_array),
            path,
            JsonbPGetDatum(append_value),
            BoolGetDatum(true)
        ));

        cur_time = GetCurrentTimestamp();
        updates = insert_vector_item(updates, 0, &cur_time);
        counter = 1;
    }
    else 
    {
        new_value = jsonb_array;
        counter = 0;
    }

    cj_len = offsetof(CrdtJsonb, updates) + VARSIZE(updates) + VARSIZE(new_value);
    cj = palloc0(cj_len);

    SET_VARSIZE(cj, cj_len);

    memmove(GET_UPDATESP(cj), updates, VARSIZE(updates));
    memmove(GET_VALUEP(cj), new_value, VARSIZE(new_value));
    cj->update_counter = counter;

    PG_RETURN_CRDTJSONB_P(cj);
}

Datum
crdt_jsonb_out(PG_FUNCTION_ARGS)
{
    CrdtJsonb *cj;
    char *str;

    cj = PG_GETARG_CRDTJSONB_P(0);

    str = JsonbToCString(NULL, &((GET_VALUEP(cj))->root), VARSIZE(GET_VALUEP(cj)));

    PG_RETURN_CSTRING(str);
}

/* SEND_RECIEVE PROTOCOL

(1) 4 bytes: update lenght (0 if jsonb array is empty)

if (1) is not zero:

(2) update lenght bytes: jsonb, which was appended
(3) 8 bytes: timestamp of this update

*/

Datum
crdt_jsonb_recv(PG_FUNCTION_ARGS)
{
    StringInfo buffer;
    CrdtJsonb *old_crdt_jsonb, *merged;
    Jsonb *accepted_jsonb;
    int32 jsonb_len;
    Timestamp update_time;

    CustomVector *updates;
    Jsonb *jsonb_array;
    char *json_str, *jsonb_array_str;
    int merged_len, counter;

    Jsonb *new_value;
    Timestamp cur_time;
    Datum path, path_datum;
    text *path_text;

    buffer = (StringInfo) PG_GETARG_POINTER(0);

    jsonb_len = pq_getmsgint(buffer, 4);

    if (jsonb_len == 0)
    {
        jsonb_array_str = CRDTJSONB_EMPTY_ARRAY;
        jsonb_array = DatumGetJsonbP(DirectFunctionCall1(jsonb_in, CStringGetDatum(jsonb_array_str)));
        updates = create_vector(2, sizeof(Timestamp));

        merged_len = offsetof(CrdtJsonb, updates) + VARSIZE(updates) + VARSIZE(jsonb_array);
        merged = palloc0(merged_len);

        SET_VARSIZE(merged, merged_len);

        memmove(GET_UPDATESP(merged), updates, VARSIZE(updates));
        memmove(GET_VALUEP(merged), jsonb_array, VARSIZE(jsonb_array));
        merged->update_counter = 0;

        PG_RETURN_CRDTJSONB_P(merged);
    }

    accepted_jsonb = (Jsonb *) palloc0(jsonb_len);

    memmove(accepted_jsonb, pq_getmsgbytes(buffer, jsonb_len), jsonb_len);
    update_time = pq_getmsgint64(buffer);

    if (fcinfo->args[3].isnull)
    {
        // in update first recv is calling for making a new tuple (for search)
        ereport(DEBUG2, errmsg("no old_crdt_jsonb"));

        json_str = DatumGetCString(DirectFunctionCall1(jsonb_out, JsonbPGetDatum(accepted_jsonb)));

        jsonb_array_str = CRDTJSONB_EMPTY_ARRAY;
        jsonb_array = DatumGetJsonbP(DirectFunctionCall1(jsonb_in, CStringGetDatum(jsonb_array_str)));
        updates = create_vector(2, sizeof(Timestamp));

        if (strcmp(json_str, CRDTJSONB_NO_INITIAL_VALUE) != 0) 
        {
            path_text = cstring_to_text("0");
            path_datum = PointerGetDatum(path_text);
            path = PointerGetDatum(construct_array(&path_datum, 1, TEXTOID, -1, false, 'i'));

            new_value = DatumGetJsonbP(DirectFunctionCall4Coll(
                jsonb_insert,
                InvalidOid,
                JsonbPGetDatum(jsonb_array),
                path,
                JsonbPGetDatum(accepted_jsonb),
                BoolGetDatum(true)
            ));

            cur_time = GetCurrentTimestamp();
            updates = insert_vector_item(updates, 0, &cur_time);
            counter = 1;
        }
        else 
        {
            new_value = jsonb_array;
            counter = 0;
        }

        merged_len = offsetof(CrdtJsonb, updates) + VARSIZE(updates) + VARSIZE(new_value);
        merged = palloc0(merged_len);

        SET_VARSIZE(merged, merged_len);

        memmove(GET_UPDATESP(merged), updates, VARSIZE(updates));
        memmove(GET_VALUEP(merged), new_value, VARSIZE(new_value));
        merged->update_counter = counter;
    }
    else
    {
        ereport(DEBUG2, errmsg("there is old_crdt_jsonb"));

        old_crdt_jsonb = PG_GETARG_CRDTJSONB_P(3);

        merged = merge(old_crdt_jsonb, accepted_jsonb, update_time);
    }

    PG_RETURN_CRDTJSONB_P(merged);
}

Datum
crdt_jsonb_send(PG_FUNCTION_ARGS)
{
    CrdtJsonb *cj;
    Jsonb *appended_jsonb;
    StringInfoData buffer;

    cj = PG_GETARG_CRDTJSONB_P(0);

    pq_begintypsend(&buffer);

    if (cj->update_counter == 0)
    {
        pq_sendint32(&buffer, 0);
    }
    else
    {
        appended_jsonb = get_last_jsonb(GET_VALUEP(cj));

        pq_sendint32(&buffer, VARSIZE(appended_jsonb));

        pq_sendbytes(&buffer, appended_jsonb, VARSIZE(appended_jsonb));

        pq_sendint64(&buffer, *((Timestamp *) get_vector_item(GET_UPDATESP(cj), cj->update_counter - 1)));
    }

    PG_RETURN_BYTEA_P(pq_endtypsend(&buffer));
}


Datum
crdt_jsonb_append(PG_FUNCTION_ARGS)
{
    CrdtJsonb *cj, *new_cj;
    Jsonb *append_value, *new_value;
    int32 new_len;

    int old_counter;
    CustomVector *updates_ptr;
    Timestamp cur_time;

    Datum path, path_datum;
    text *path_text;

    cj = PG_GETARG_CRDTJSONB_P(0);
    append_value = PG_GETARG_JSONB_P(1);


    if (!JB_ROOT_IS_ARRAY(GET_VALUEP(cj)))
    {
        ereport(ERROR,
                (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                    errmsg("jsonb root must be array")));
    }

    path_text = cstring_to_text("-1");
    path_datum = PointerGetDatum(path_text);
    path = PointerGetDatum(construct_array(&path_datum, 1, TEXTOID, -1, false, 'i'));

    new_value = DatumGetJsonbP(DirectFunctionCall4Coll(
        jsonb_insert,
        InvalidOid,
        JsonbPGetDatum(GET_VALUEP(cj)),
        path,
        JsonbPGetDatum(append_value),
        BoolGetDatum(true)
    ));

    old_counter = cj->update_counter;
    updates_ptr = GET_UPDATESP(cj);

    cur_time = GetCurrentTimestamp();
    updates_ptr = insert_vector_item(updates_ptr, old_counter, &cur_time);

    new_len = offsetof(CrdtJsonb, updates) + VARSIZE(updates_ptr) + VARSIZE(new_value);
    new_cj = palloc0(new_len);

    SET_VARSIZE(new_cj, new_len);

    memmove(GET_UPDATESP(new_cj), updates_ptr, VARSIZE(updates_ptr));
    memmove(GET_VALUEP(new_cj), new_value, VARSIZE(new_value));
    new_cj->update_counter = old_counter + 1;

    PG_RETURN_CRDTJSONB_P(new_cj);
}

CrdtJsonb *
merge(CrdtJsonb *cj_old, const Jsonb *appended_jsonb, const Timestamp update_time)
{
    CrdtJsonb *new_cj;
    Jsonb *new_value;
    CustomVector *updates_ptr;
    int32 index, new_len;
    int32 old_counter;

    Datum path, path_datum;
    text *path_text;
    char path_str[12];

    index = find_index_for_insert(cj_old, update_time);

    if (index == -1)
    {
        ereport(INFO, (errmsg("similar timestamps")));
        return cj_old;
    }

    snprintf(path_str, sizeof(path_str), "%d", index);
    path_text = cstring_to_text(path_str);
    path_datum = PointerGetDatum(path_text);
    path = PointerGetDatum(construct_array(&path_datum, 1, TEXTOID, -1, false, 'i'));


    new_value = DatumGetJsonbP(DirectFunctionCall4Coll(
        jsonb_insert,
        InvalidOid,
        JsonbPGetDatum(GET_VALUEP(cj_old)),
        path,
        JsonbPGetDatum(appended_jsonb),
        BoolGetDatum(false)
    ));

    old_counter = cj_old->update_counter;

    updates_ptr = GET_UPDATESP(cj_old);

    updates_ptr = insert_vector_item(updates_ptr, index, &update_time);

    new_len = offsetof(CrdtJsonb, updates) + VARSIZE(updates_ptr) + VARSIZE(new_value);
    new_cj = palloc0(new_len);
    SET_VARSIZE(new_cj, new_len);

    memmove(GET_UPDATESP(new_cj), updates_ptr, VARSIZE(updates_ptr));
    memmove(GET_VALUEP(new_cj), new_value, VARSIZE(new_value));
    new_cj->update_counter = old_counter + 1;

    return new_cj;
}

Datum
get_jsonb(PG_FUNCTION_ARGS)
{
    CrdtJsonb *cj;

    cj = PG_GETARG_CRDTJSONB_P(0);

    PG_RETURN_JSONB_P(GET_VALUEP(cj));
}

static Jsonb *
get_last_jsonb(Jsonb *jsonb_array)
{
    Jsonb *last_jsonb;

    last_jsonb = JsonbValueToJsonb(getIthJsonbValueFromContainer(&jsonb_array->root, JB_ROOT_COUNT(jsonb_array) - 1));

    return last_jsonb;
}

static int32
find_index_for_insert(const CrdtJsonb *cj, const Timestamp update_time)
{
    CustomVector *updates;
    int i;
    Timestamp i_time;
    int cmp_res;

    updates = GET_UPDATESP(cj);

    for (i = cj->update_counter - 1; i >= 0; --i)
    {
        i_time = *((Timestamp *) get_vector_item(updates, i));
        cmp_res = timestamp_cmp_internal(i_time, update_time);
        ereport(DEBUG2, errmsg("i_time = %ld, updat_time = %ld, cmp_res = %d", i_time, update_time, cmp_res));
        if (cmp_res == -1)
        {
            return i + 1;
        }
        if (cmp_res == 0)
        {
            return -1; //for idempotency
        }
    }
    return 0;
}


/*------------------------------------------VECTOR------------------------------------------*/

static CustomVector *
create_vector(const uint32 initial_capacity, const uint32 item_size)
{
    CustomVector *vector;

    vector = palloc0(sizeof(CustomVector) + initial_capacity * item_size);

    vector->size = 0;
    vector->capacity = initial_capacity;
    vector->item_size = item_size;

    SET_VARSIZE(vector, sizeof(CustomVector) + initial_capacity * item_size);

    return vector;
}

static CustomVector *
resize_vector(CustomVector *vector)
{
    CustomVector *new_vector;
    uint32 new_capacity;

    if (vector == NULL)
    {
        ereport(ERROR, errmsg("vector is null"));
    }

    new_capacity = vector->capacity + VECTOR_MAGNIFICATION_NUMBER;
    new_vector = palloc0((sizeof(CustomVector) + new_capacity * vector->item_size));

    new_vector->size = vector->size;
    new_vector->item_size = vector->item_size;
    memmove((char *) new_vector + sizeof(CustomVector), (char *) vector + sizeof(CustomVector),
            vector->size * vector->item_size);
    new_vector->capacity = new_capacity;

    SET_VARSIZE(new_vector, sizeof(CustomVector) + new_capacity * new_vector->item_size);

    return new_vector;
}

static void *
get_vector_item(const CustomVector *vector, const uint32 index)
{
    if (index >= vector->size)
    {
        ereport(ERROR, errmsg("index out of bounds"));
    }

    return (void *) ((char *) vector + sizeof(CustomVector) + index * vector->item_size);
}

static void
set_vector_item(const CustomVector *vector, const uint32 index, const void* item)
{
    if (index >= vector->size)
    {
        ereport(ERROR, errmsg("index out of bounds"));
    }

    memcpy((char*) vector + sizeof(CustomVector) + index * vector->item_size, item, vector->item_size);
}

static CustomVector *
insert_vector_item(CustomVector *vector, const uint32 index, const void *item)
{
    char *start;

    if (index > vector->size)
    {
        ereport(ERROR, errmsg("index out of bounds"));
    }

    if (vector->size == vector->capacity)
    {
        vector = resize_vector(vector);
    }

    start = (char *) vector + sizeof(CustomVector) + index * vector->item_size;
    memmove(start + vector->item_size, start, (vector->size - index) * vector->item_size);
    memmove(start, item, vector->item_size);

    vector->size++;
    return vector;
}
