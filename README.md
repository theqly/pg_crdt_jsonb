# CRDT for JSONB

Conflict-Free Replicated Datatypes (CRDTs) are a family of data structures that support concurrent local modifications in different replicas in a way that there are no conflicts when replicas are merged. It also ensures that the state of each replica will eventually converge. A JSONB CRDT ensures that a data structure consistent with the JSONB object, which is array at the top level.
It can be used with a logical replication, where primary nodes connected with each other by bidirectional replication and replica nodes connected with each primary node.

# API

- **creation**: it is possible to create an empty array or pass a string that will be converted to jsonb and placed in the array at index zero when initializing an object
- **insertion**: you can only insert new elements at the end of the array using the function **`crdt_jsonb_append(crdt_jsonb obj1, jsonb obj2)`**, where first argument is an crdt_jsonb object, where the new jsonb will be added, specified by second argument
- **cast** is supported from crdt_jsonb to jsonb

# Restrictions!!!

- it is forbidden to specify the **`copy=true`** parameter when creating a subscription 
- jsonb must be an array at the top level
- using **`crdt_jsonb_append(crdt_jsonb_append(...), obj)`** or more than one append in one transaction is prohibited, because all updates will be lost expect external jsonb
- there is a patch only for version 16 stable of Postgres at the moment

# When to use
- When you need to organize bidirectional replication using Jsonb.
- When communication between nodes takes a long time due to the transfer of large objects.
- When disk space for meta information is not an issue.

# Technology

For each element in the top-level array, its own Timestamp is created. The timestamp of each element is stored in a auxiliary array at the same index as the Jsonb element.
With logical replication, the crdt_jsonb object is not sent in its entirety, but only its last element (that is the last update). When receiving data the comparison is made from the end of the Timestamp array, and the resulting element is inserted at the same index as its Timestamp.
#### IMPORTANT NOTICE!
forwarding deltas only works if the **`binary = true`** parameter is enabled when creating a subscription.

# Delta operations in more detail

A record with the new value is written to wal after making "append". A separate process for logical replication for a specific table will call the send function of our type and send the result along the replication channel between nodes. The corresponding process on the receiver node will receive the byte stream from the channel and will decode it. Then the recv function will be called for our type. Thanks to the patch for the postgres source code, the recv function will receive an additional parameter: the value that lies at the receiver node in the table cell that we are going to change using the recv function.

![Schema](./replication.webp)


# Installation

```bash
git clone https://gitpglab.postgrespro.ru/pgpro-perf/jsonb-crdt.git
`````

```bash
make
````

```bash
sudo make install
````

Also you need to make some changes in postgres code. Go to the folder with the postgres source code (where the src folder is located) and run the next command (replace the path to the file with patch, which is located in the newly cloned directory)

```bash
git apply pg_16_stable_delta_receive.patch
````

# Usage

In postgresql.conf file set parameters:

**`shared_preload_libraries = 'pg_crdt_jsonb'`**

**`wal_level = logical`**

After running the databases, do the following:

### NODE1

```bash
create extension pg_crdt_jsonb;
````

```bash
create table table_name(id int primary key, data crdt_jsonb);
````

```bash
create publication pub1_name for table table_name;
````

```bash
create subscription sub1_name connection `host=node2_host port=node2_port dbname=postgres` publication pub2_name with (create_slot=true, copy_data = false, origin = none, binary = true);
````

### NODE2

```bash
create extension pg_crdt_jsonb;
````

```bash
create table table_name(id int primary key, data crdt_jsonb);
````

```bash
create publication pub2_name for table table_name;
````

```bash
create subscription sub2_name connection `host=node1_host port=node1_port dbname=postgres` publication pub1_name with (create_slot=true, copy_data = false, origin = none, binary = true);
````

### Commands

Use next command for making empty array

```bash
insert into table_name values(1, ``);
````

Use next command for making array with first element

```bash
insert into table_name values(0, `{"key" : "value"}`);
````

Use next command for appending

```bash
update table_name set data = crdt_jsonb_append(data, `{"key" : "value"}`) where id = 0;
````

# TODO
- Removing old Timestamp array elements whose Json elements are already precisely synchronized
- Expand functionality, add deletion and change of elements
- Make common numbering for nodes to handle the case of appending from different nodes with the same timestamps