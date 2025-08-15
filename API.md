# API Documentation

This document covers all endpoint operations.

If you discover that some topic related to the behavior of the endpoint operation isn't documented, please open an issue in the repository.

Throughout the document you'll come across identifiers such as `<version>`, `<string>`, `<integer>`, etc. These identifiers represent placeholders. Users are expected to replace placeholders with appropriate values.

## Authentication Operations

### Scheme: Basic

#### Request

```bash
curl -X POST -u <username>:<password> http://localhost:<port>/irods-http-api/<version>/authenticate
```

`<version>` must match the format, **X.Y.Z**, where **X** is the major version number, **Y** is the minor version number, and **Z** is the patch version number.

#### Response

A string representing a bearer token that can be used to execute operations as the authenticated user.

Tokens obtained via this authentication scheme have a finite lifetime. The lifetime of a token is defined in the configuration file at `/http_server/authentication/basic/timeout_in_seconds`. **Use of the token does NOT extend its lifetime**.

Attempting to use an expired or invalid token will result in a response containing a status code of **401 Unauthorized**. Checking for this status code is key to detecting when the client needs to reauthenticate.

Reauthentication can be performed at anytime and will result in a brand new token. This does NOT invalidate previously acquired tokens.

### Scheme: OpenID Connect (OIDC)

For authenticating with OpenID Connect, the HTTP API can be run as an OAuth Protected Resource.

While running in this mode, the HTTP API does not provide any grants to which you can authenticate.
Instead, you must implement an OAuth client yourself, authenticate, and provide an access token as a
Bearer token when querying the HTTP API endpoints.

In order for the access token to be accepted, two conditions must be met:
1. The OpenID Provider recognizes the token as valid and active.
2. The token must be able to be mapped to a local zone user using a valid user mapping plugin.

If these conditions are met, then the access token will be accepted, and the action will be carried out as the mapped
user.

## Collection Operations

### create

Creates a new collection.

#### Request

HTTP Method: POST

```bash
curl http://localhost:<port>/irods-http-api/<version>/collections \
    -H 'Authorization: Bearer <token>' \
    --data-urlencode 'op=create' \
    --data-urlencode 'lpath=<string>' \
    --data-urlencode 'create-intermediates=<integer>' # 0 or 1. Defaults to 0. Optional.
```

#### Response

If an HTTP status code of 200 is returned, the body of the response will contain JSON. Its structure is shown below.

```js
{
    "irods_response": {
        "status_code": 0
        "status_message": "string" // Optional
    },
    "created": false // Conditionally available. Check "irods_response/status_code" first.
}
```

If there was an error, expect an HTTP status code in either the 4XX or 5XX range.

### remove

Removes a collection.

#### Request

HTTP Method: POST

```bash
curl http://localhost:<port>/irods-http-api/<version>/collections \
    -H 'Authorization: Bearer <token>' \
    --data-urlencode 'op=remove' \
    --data-urlencode 'lpath=<string>' \
    --data-urlencode 'recurse=<integer>' \ # 0 or 1. Defaults to 0. Optional.
    --data-urlencode 'no-trash=<integer>' # 0 or 1. Defaults to 0. Optional.
```

If `recurse` is set to 1, the contents of the collection will be removed. If `no-trash` is set to 1, the collection is permanently removed.

#### Response

If an HTTP status code of 200 is returned, the body of the response will contain JSON. Its structure is shown below.

```js
{
    "irods_response": {
        "status_code": 0
        "status_message": "string" // Optional
    }
}
```

If there was an error, expect an HTTP status code in either the 4XX or 5XX range.

### stat

Returns information about a collection.

#### Request

HTTP Method: GET

```bash
curl http://localhost:<port>/irods-http-api/<version>/collections \
    -H 'Authorization: Bearer <token>' \
    --data-urlencode 'op=stat' \
    --data-urlencode 'lpath=<string>' \
    --data-urlencode 'ticket=<string>' \ # Optional.
    -G
```

If `ticket` is passed a valid ticket string, it will be enabled before carrying out the operation.

#### Response

If an HTTP status code of 200 is returned, the body of the response will contain JSON. Its structure is shown below.

```js
{
    "irods_response": {
        "status_code": 0
        "status_message": "string" // Optional
    },
    "type": "string",
    "inheritance_enabled": false,
    "permissions": [
        {
            "name": "string",
            "zone": "string",
            "type": "string",
            "perm": "string"
        }
    ],
    "registered": false,
    "modified_at": 0
}
```

If there was an error, expect an HTTP status code in either the 4XX or 5XX range.

### list

Returns the contents of a collection.

#### Request

HTTP Method: GET

```bash
curl http://localhost:<port>/irods-http-api/<version>/collections \
    -H 'Authorization: Bearer <token>' \
    --data-urlencode 'op=list' \
    --data-urlencode 'lpath=<string>' \
    --data-urlencode 'recurse=<integer>' \ # 0 or 1. Defaults to 0. Optional.
    --data-urlencode 'ticket=<string>' \ # Optional
    -G
```

#### Response

If an HTTP status code of 200 is returned, the body of the response will contain JSON. Its structure is shown below.

```js
{
    "irods_response": {
        "status_code": 0
        "status_message": "string" // Optional
    },
    "entries": [
        "string",
        "string",
        "string"
    ]
}
```

If there was an error, expect an HTTP status code in either the 4XX or 5XX range.

### set_permission

Sets the permission of a user or group on a collection.

#### Request

HTTP Method: POST

```bash
curl http://localhost:<port>/irods-http-api/<version>/collections \
    -H 'Authorization: Bearer <token>' \
    --data-urlencode 'op=set_permission' \
    --data-urlencode 'lpath=<string>' \
    --data-urlencode 'entity-name=<string>' \ # The name of a user or group.
    --data-urlencode 'permission=<string>' \
    --data-urlencode 'admin=<integer>' # 0 or 1. Defaults to 0. Execute as a rodsadmin. Optional.
```

The following permission levels are supported:
- own
- delete_object
- modify_object _(equivalent to write)_
- create_object
- delete_metadata
- modify_metadata
- create_metadata
- read_object _(equivalent to read)_
- read_metadata
- null

The following legacy permission levels are supported as well:
- own
- write
- read
- null

#### Response

If an HTTP status code of 200 is returned, the body of the response will contain JSON. Its structure is shown below.

```js
{
    "irods_response": {
        "status_code": 0
        "status_message": "string" // Optional
    }
}
```

If there was an error, expect an HTTP status code in either the 4XX or 5XX range.

### set_inheritance

Enable or disable inheritance on a collection.

#### Request

HTTP Method: POST

```bash
curl http://localhost:<port>/irods-http-api/<version>/collections \
    -H 'Authorization: Bearer <token>' \
    --data-urlencode 'op=set_inheritance' \
    --data-urlencode 'lpath=<string>' \
    --data-urlencode 'enable=<integer>' \ # 0 or 1.
    --data-urlencode 'admin=<integer>' # 0 or 1. Defaults to 0. Optional.
```

#### Response

If an HTTP status code of 200 is returned, the body of the response will contain JSON. Its structure is shown below.

```js
{
    "irods_response": {
        "status_code": 0
        "status_message": "string" // Optional
    }
}
```

If there was an error, expect an HTTP status code in either the 4XX or 5XX range.

### modify_permissions

Adjust permissions for multiple users and groups on a collection atomically.

#### Request

HTTP Method: POST

```bash
curl http://localhost:<port>/irods-http-api/<version>/collections \
    -H 'Authorization: Bearer <token>' \
    --data-urlencode 'op=modify_permissions' \
    --data-urlencode 'lpath=<string>' \
    --data-urlencode 'operations=<json_object>' \
    --data-urlencode 'admin=<integer>' # 0 or 1. Defaults to 0. Execute as a rodsadmin. Optional.
```

The JSON object passed to the `operations` parameter must have the following structure:

```js
[
    {
        "entity_name": "string", // The name of a user or group.
        "acl": "string" // null, read, write, or own.
    },

    // Additional ACL operations ...
]
```

#### Response

If an HTTP status code of 200 is returned, the body of the response will contain JSON. Its structure is shown below.

```js
{
    "irods_response": {
        "status_code": 0
        "status_message": "string" // Optional
    }
}
```

If there was an error, expect an HTTP status code in either the 4XX or 5XX range. If an operation failed, the `irods_response` object will include an additional property called `failed_operation`. The structure is shown below.

```js
{
    "irods_response": {
        "status_code": 0
        "status_message": "string", // Optional
        "failed_operation": {
            "error_message": "string",
            "operation": {
                "entity_name": "string", // The name of a user or group.
                "acl": "string" // null, read, write, or own.
            },
            "operation_index": 0
        }
    }
}
```

### modify_metadata

Adjust multiple AVUs on a collection.

#### Request

HTTP Method: POST

```bash
curl http://localhost:<port>/irods-http-api/<version>/collections \
    -H 'Authorization: Bearer <token>' \
    --data-urlencode 'op=modify_metadata' \
    --data-urlencode 'lpath=<string>' \
    --data-urlencode 'operations=<json_object>' \
    --data-urlencode 'admin=<integer>' # 0 or 1. Defaults to 0. Execute as a rodsadmin. Optional.
```

The JSON object passed to the `operations` parameter must have the following structure:

```js
[
    {
        "operation": "string", // add or remove.
        "attribute": "string",
        "value": "string",
        "units": "string" // Optional.
    },

    // Additional AVU operations ...
]
```

#### Response

If an HTTP status code of 200 is returned, the body of the response will contain JSON. Its structure is shown below.

```js
{
    "irods_response": {
        "status_code": 0
        "status_message": "string" // Optional
    }
}
```

If there was an error, expect an HTTP status code in either the 4XX or 5XX range. If an operation failed, the `irods_response` object will include an additional property called `failed_operation`. The structure is shown below.

```js
{
    "irods_response": {
        "status_code": 0
        "status_message": "string", // Optional
        "failed_operation": {
            "error_message": "string",
            "operation": {
                "operation": "string", // add or remove.
                "attribute": "string",
                "value": "string",
                "units": "string" // Optional.
            },
            "operation_index": 0
        }
    }
}
```

### rename

Renames or moves a collection.

#### Request

HTTP Method: POST

```bash
curl http://localhost:<port>/irods-http-api/<version>/collections \
    -H 'Authorization: Bearer <token>' \
    --data-urlencode 'op=rename' \
    --data-urlencode 'old-lpath=<string>' \
    --data-urlencode 'new-lpath=<string>'
```

#### Response

If an HTTP status code of 200 is returned, the body of the response will contain JSON. Its structure is shown below.

```js
{
    "irods_response": {
        "status_code": 0
        "status_message": "string" // Optional
    }
}
```

If there was an error, expect an HTTP status code in either the 4XX or 5XX range.

### touch

Updates the mtime of an existing collection.

#### Request

HTTP Method: POST

```bash
curl http://localhost:<port>/irods-http-api/<version>/collections \
    -H 'Authorization: Bearer <token>' \
    --data-urlencode 'op=touch' \
    --data-urlencode 'lpath=<string>' \ # Absolute logical path to a collection.
    --data-urlencode 'seconds-since-epoch=<integer>' \ # The mtime to assign to the collection. Optional.
    --data-urlencode 'reference=<string>' # The absolute logical path of an object whose mtime will be copied to the collection. Optional.
```

#### Response

If an HTTP status code of 200 is returned, the body of the response will contain JSON. Its structure is shown below.

```js
{
    "irods_response": {
        "status_code": 0
        "status_message": "string" // Optional
    }
}
```

If there was an error, expect an HTTP status code in either the 4XX or 5XX range.

## Data Object Operations

### touch

Updates the mtime of an existing data object or creates a new data object if it does not exist.

#### Request

HTTP Method: POST

```bash
curl http://localhost:<port>/irods-http-api/<version>/data-objects \
    -H 'Authorization: Bearer <token>' \
    --data-urlencode 'op=touch' \
    --data-urlencode 'lpath=<string>' \ # Absolute logical path to a data object.
    --data-urlencode 'no-create=<integer>' \ # 0 or 1. Defaults to 0. If set to 1, no data objects will be created. Optional.
    --data-urlencode 'replica-number=<integer>' \ # The replica to update. The replica must exist. Optional.
    --data-urlencode 'leaf-resource=<string>' \ # The resource holding an existing replica. If it does not exist, it will be created on the specified resource. Optional.
    --data-urlencode 'seconds-since-epoch=<integer>' \ # The mtime to assign to the replica. Optional.
    --data-urlencode 'reference=<string>' # The absolute logical path of an object whose mtime will be copied to the data object. Optional.
```

#### Response

If an HTTP status code of 200 is returned, the body of the response will contain JSON. Its structure is shown below.

```js
{
    "irods_response": {
        "status_code": 0
        "status_message": "string" // Optional
    }
}
```

If there was an error, expect an HTTP status code in either the 4XX or 5XX range.

### truncate

Resizes an existing replica.

#### Request

HTTP Method: POST

```bash
curl http://localhost:<port>/irods-http-api/<version>/data-objects \
    -H 'Authorization: Bearer <token>' \
    --data-urlencode 'op=truncate' \
    --data-urlencode 'lpath=<string>' \ # Absolute logical path to a data object.
    --data-urlencode 'resource=<string>' \ # The resource holding an existing replica. Optional.
    --data-urlencode 'replica-number=<integer>' \ # The replica to resize. Optional.
    --data-urlencode 'admin=<integer>' # 0 or 1. Defaults to 0. Execute as a rodsadmin. Optional.
```

#### Response

If an HTTP status code of 200 is returned, the body of the response will contain JSON. Its structure is shown below.

```js
{
    "irods_response": {
        "status_code": 0
        "status_message": "string", // Optional
        "additional_info": {
            "resource_hierarchy": "string",
            "replica_number": 0,
            "message": "string"
        }
    }
}
```

If there was an error, expect an HTTP status code in either the 4XX or 5XX range.

### remove

Removes a data object or unregisters all replicas.

#### Request

HTTP Method: POST

```bash
curl http://localhost:<port>/irods-http-api/<version>/data-objects \
    -H 'Authorization: Bearer <token>' \
    --data-urlencode 'op=remove' \
    --data-urlencode 'lpath=<string>' \ # Absolute logical path to a data object.
    --data-urlencode 'catalog-only=<integer>' \ # 0 or 1. If set to 1, removes only the catalog entry.
    --data-urlencode 'no-trash=<integer>' \ # 0 or 1. Defaults to 0. If set to 1, permanently deletes the data object. Optional.
    --data-urlencode 'admin=<integer>' # 0 or 1. Defaults to 0. Execute as a rodsadmin. Optional.
```

`catalog-only` and `no-trash` are mutually exclusive parameters. Setting both to 1 will result in an error.

`catalog-only` requires rodsadmin level privileges. This requirement can be relaxed by adjusting the iRODS server's policy.

#### Response

If an HTTP status code of 200 is returned, the body of the response will contain JSON. Its structure is shown below.

```js
{
    "irods_response": {
        "status_code": 0
        "status_message": "string" // Optional
    }
}
```

If there was an error, expect an HTTP status code in either the 4XX or 5XX range.

### calculate_checksum

Calculates the checksum for a data object.

#### Request

HTTP Method: POST

```bash
curl http://localhost:<port>/irods-http-api/<version>/data-objects \
    -H 'Authorization: Bearer <token>' \
    --data-urlencode 'op=calculate_checksum' \
    --data-urlencode 'lpath=<string>' \ # Absolute logical path to a data object.
    --data-urlencode 'resource=<string>' \ # The resource holding the target replica. Optional.
    --data-urlencode 'replica-number=<integer>' \ # The replica number of the target replica. Optional.
    --data-urlencode 'force=<integer>' \ # 0 or 1. Defaults to 0. Overwrite the existing checksum. Optional.
    --data-urlencode 'all=<integer>' \ # 0 or 1. Defaults to 0. Calculate checksums for all replicas. Optional.
    --data-urlencode 'admin=<integer>' # 0 or 1. Defaults to 0. Execute as a rodsadmin. Optional.
```

`resource`, `replica-number`, and `all` are mutually exclusive parameters.

#### Response

If an HTTP status code of 200 is returned, the body of the response will contain JSON. Its structure is shown below.

```js
{
    "irods_response": {
        "status_code": 0
        "status_message": "string" // Optional
    },
    "checksum": "string"
}
```

If there was an error, expect an HTTP status code in either the 4XX or 5XX range.

### verify_checksum

Verifies the checksum information for a data object.

#### Request

HTTP Method: GET

```bash
curl http://localhost:<port>/irods-http-api/<version>/data-objects \
    -H 'Authorization: Bearer <token>' \
    --data-urlencode 'op=verify_checksum' \
    --data-urlencode 'lpath=<string>' \ # Absolute logical path to a data object.
    --data-urlencode 'resource=<string>' \ # The resource holding the target replica. Optional.
    --data-urlencode 'replica-number=<integer>' \ # The replica number of the target replica. Optional.
    --data-urlencode 'compute-checksums=<integer>' \ # 0 or 1. Defaults to 1. Can be used to skip the checksum calculation step. Optional.
    --data-urlencode 'admin=<integer>' \ # 0 or 1. Defaults to 0. Execute as a rodsadmin. Optional.
    -G
```

#### Response

If an HTTP status code of 200 is returned, the body of the response will contain JSON. Its structure is shown below.

```js
{
    "irods_response": {
        "status_code": 0
        "status_message": "string" // Optional
    },
    "results": {
        // Verification results. This object only exists if the operation found inconsistencies
        // between what's in storage and what's in the catalog.
    },
    "r_error_info": [
        {
            "status": 0,
            "message": "string"
        },

        // Additional elements ...
    ]
}
```

If there was an error, expect an HTTP status code in either the 4XX or 5XX range.

### stat

Returns information about a data object.

#### Request

HTTP Method: GET

```bash
curl http://localhost:<port>/irods-http-api/<version>/data-objects \
    -H 'Authorization: Bearer <token>' \
    --data-urlencode 'op=stat' \
    --data-urlencode 'lpath=<string>' \ # Absolute logical path to a data object.
    --data-urlencode 'ticket=<string>' \ # The ticket to enable before stat'ing the data object. Optional.
    -G
```

#### Response

If an HTTP status code of 200 is returned, the body of the response will contain JSON. Its structure is shown below.

```js
{
    "irods_response": {
        "status_code": 0
        "status_message": "string" // Optional
    },
    "type": "string",
    "permissions": [
        {
            "name": "string",
            "zone": "string",
            "type": "string",
            "perm": "string"
        }
    ],
    "size": 0,
    "checksum": "string",
    "registered": false,
    "modified_at": 0
}
```

If there was an error, expect an HTTP status code in either the 4XX or 5XX range.

### rename

Renames or moves a data object.

#### Request

HTTP Method: POST

```bash
curl http://localhost:<port>/irods-http-api/<version>/data-objects \
    -H 'Authorization: Bearer <token>' \
    --data-urlencode 'op=rename' \
    --data-urlencode 'old-lpath=<string>' \ # The absolute logical path of the data object to rename.
    --data-urlencode 'new-lpath=<string>' # The new absolute logical path of the data object.
```

#### Response

If an HTTP status code of 200 is returned, the body of the response will contain JSON. Its structure is shown below.

```js
{
    "irods_response": {
        "status_code": 0
        "status_message": "string" // Optional
    }
}
```

If there was an error, expect an HTTP status code in either the 4XX or 5XX range.

### copy

Copies a data object.

#### Request

HTTP Method: POST

```bash
curl http://localhost:<port>/irods-http-api/<version>/data-objects \
    -H 'Authorization: Bearer <token>' \
    --data-urlencode 'op=copy' \
    --data-urlencode 'src-lpath=<string>' \ # The absolute logical path of the data object to copy.
    --data-urlencode 'dst-lpath=<string>' \ # The absolute logical path of a new or existing data object.
    --data-urlencode 'src-resource=<string>' \ # The name of the root resource to copy from. Optional.
    --data-urlencode 'dst-resource=<string>' \ # The name of the root resource to copy to. Optional.
    --data-urlencode 'overwrite=<integer>' # 0 or 1. Defaults to 0. Optional. Instructs the server to replace the existing data object.
```

#### Response

If an HTTP status code of 200 is returned, the body of the response will contain JSON. Its structure is shown below.

```js
{
    "irods_response": {
        "status_code": 0
        "status_message": "string" // Optional
    }
}
```

If there was an error, expect an HTTP status code in either the 4XX or 5XX range.

### replicate

Replicates an existing replica from one resource to another resource.

#### Request

HTTP Method: POST

```bash
curl http://localhost:<port>/irods-http-api/<version>/data-objects \
    -H 'Authorization: Bearer <token>' \
    --data-urlencode 'op=replicate' \
    --data-urlencode 'lpath=<string>' \ # Absolute logical path to a data object.
    --data-urlencode 'src-resource=<string>' \ # The resource to replicate from.
    --data-urlencode 'dst-resource=<string>' \ # The resource to replicate to.
    --data-urlencode 'admin=<integer>' # 0 or 1. Defaults to 0. Execute as a rodsadmin. Optional.
```

#### Response

If an HTTP status code of 200 is returned, the body of the response will contain JSON. Its structure is shown below.

```js
{
    "irods_response": {
        "status_code": 0
        "status_message": "string" // Optional
    }
}
```

If there was an error, expect an HTTP status code in either the 4XX or 5XX range.

### trim

Trims an existing replica or removes its catalog entry.

#### Request

HTTP Method: POST

```bash
curl http://localhost:<port>/irods-http-api/<version>/data-objects \
    -H 'Authorization: Bearer <token>' \
    --data-urlencode 'op=trim' \
    --data-urlencode 'lpath=<string>' \ # Absolute logical path to a data object.
    --data-urlencode 'replica-number=<integer>' \ # The replica number identifying the replica to trim.
    --data-urlencode 'catalog-only=<integer>' \ # 0 or 1. If set to 1, removes only the catalog entry. Optional.
    --data-urlencode 'admin=<integer>' # 0 or 1. Defaults to 0. Execute as a rodsadmin. Optional.
```

#### Response

If an HTTP status code of 200 is returned, the body of the response will contain JSON. Its structure is shown below.

```js
{
    "irods_response": {
        "status_code": 0
        "status_message": "string" // Optional
    }
}
```

If there was an error, expect an HTTP status code in either the 4XX or 5XX range.

### register

Registers a new data object and/or replica into the catalog.

This operation may require rodsadmin level privileges depending on the configuration of the iRODS zone. Contact the administrator of the iRODS zone to be sure non-rodsadmin users are allowed to execute this operation.

#### Request

HTTP Method: POST

```bash
curl http://localhost:<port>/irods-http-api/<version>/data-objects \
    -H 'Authorization: Bearer <token>' \
    --data-urlencode 'op=register' \
    --data-urlencode 'lpath=<string>' \ # Absolute logical path to a data object.
    --data-urlencode 'ppath=<string>' \ # Absolute physical path to file on the iRODS server.
    --data-urlencode 'resource=<string>' \ # The resource which will own the replica.
    --data-urlencode 'as-additional-replica=<integer>' \ # 0 or 1. Defaults to 0. Register as an additional replica for an existing data object. Optional.
    --data-urlencode 'data-size=<integer>' \ # The size of the replica in bytes. Optional.
    --data-urlencode 'checksum=<string>' \ # The checksum to associate with the replica. Optional.
```

#### Response

If an HTTP status code of 200 is returned, the body of the response will contain JSON. Its structure is shown below.

```js
{
    "irods_response": {
        "status_code": 0
        "status_message": "string" // Optional
    }
}
```

If there was an error, expect an HTTP status code in either the 4XX or 5XX range.

### read

Reads bytes from a data object.

#### Request

HTTP Method: GET

```bash
curl http://localhost:<port>/irods-http-api/<version>/data-objects \
    -H 'Authorization: Bearer <token>' \
    --data-urlencode 'op=read' \
    --data-urlencode 'lpath=<string>' \ # Absolute logical path to a data object.
    --data-urlencode 'resource=<string>' \ # The root resource holding the replica to read from. Optional.
    --data-urlencode 'replica-number=<integer>' \ # The replica number of the replica to read from. Optional.
    --data-urlencode 'offset=<integer>' \ # Number of bytes to skip. Defaults to 0. Optional.
    --data-urlencode 'count=<integer>' \ # Number of bytes to read. Optional.
    --data-urlencode 'ticket=<string>' \ # The ticket to enable before reading the data object. Optional.
    -G
```

`resource` and `replica-number` are mutually exclusive parameters. The behavior of the operation is unspecified if both parameters are provided.

#### Response

If an HTTP status code of 200 is returned, the body of the response will contain the bytes read from the data object.

If there was an error, expect an HTTP status code in either the 4XX or 5XX range.

### write

Writes bytes to a data object.

This operation supports two methods of sending data to the HTTP API server. **Method 2** is the recommended method.

#### Request

HTTP Method: POST

##### Method 1

```bash
curl http://localhost:<port>/irods-http-api/<version>/data-objects \
    -H 'Authorization: Bearer <token>' \
    [-F,--data-urlencode] 'op=write' \
    [-F,--data-urlencode] 'lpath=<string>' \ # Absolute logical path to a data object.
    [-F,--data-urlencode] 'resource=<string>' \ # The root resource holding the replica to write to. Optional.
    [-F,--data-urlencode] 'replica-number=<integer>' \ # The replica number of the replica to write to. Cannot be negative if specified. Optional.
    [-F,--data-urlencode] 'offset=<integer>' \ # Number of bytes to skip. Defaults to 0. Optional.
    [-F,--data-urlencode] 'truncate=<integer>' \ # 0 or 1. Defaults to 1. Truncates the data object before writing. Optional.
    [-F,--data-urlencode] 'append=<integer>' \ # 0 or 1. Defaults to 0. Appends the bytes to the data object. Optional.
    [-F,--data-urlencode] 'bytes=<binary_data>;type=application/octet-stream' \ # The bytes to write.
    [-F,--data-urlencode] 'parallel-write-handle=<string>' \ # The handle to use when writing in parallel. Optional.
    [-F,--data-urlencode] 'stream-index=<integer>' # The stream to use when writing in parallel. Optional.
```

`resource` and `replica-number` are mutually exclusive parameters. The behavior of the operation is unspecified if both parameters are provided.

This method is the original implementation. It sends all information via the HTTP request body. The HTTP API server will buffer the full request before processing it.

When sending large amounts of data or writing in parallel, multipart/form-data (`-F`) is recommended over application/x-www-form-urlencoded (`--data-urlencode`) as the Content-Type.

`parallel-write-handle` and `stream-index` only apply when writing to a replica in parallel. To obtain a parallel-write-handle, see [parallel_write_init](#parallel_write_init).

##### Method 2

```bash
curl http://localhost:<port>/irods-http-api/<version>/data-objects \
    -H 'Authorization: Bearer <token>' \
    -H 'irods-api-request-op=write' \
    -H 'irods-api-request-lpath=<string>' \ # Absolute logical path to a data object.
    -H 'irods-api-request-resource=<string>' \ # The root resource holding the replica to write to. Optional.
    -H 'irods-api-request-replica-number=<integer>' \ # The replica number of the replica to write to. Cannot be negative if specified. Optional.
    -H 'irods-api-request-offset=<integer>' \ # Number of bytes to skip. Defaults to 0. Optional.
    -H 'irods-api-request-truncate=<integer>' \ # 0 or 1. Defaults to 1. Truncates the data object before writing. Optional.
    -H 'irods-api-request-append=<integer>' \ # 0 or 1. Defaults to 0. Appends the bytes to the data object. Optional.
    -H 'irods-api-request-parallel-write-handle=<string>' \ # The handle to use when writing in parallel. Optional.
    -H 'irods-api-request-stream-index=<integer>' \ # The stream to use when writing in parallel. Optional.
    --data-binary '<bytes>' # The bytes to write.
```

`irods-api-request-resource` and `irods-api-request-replica-number` are mutually exclusive parameters. The behavior of the operation is unspecified if both parameters are provided.

Information which describes the operation is sent via HTTP headers and the data to write to the data object is sent in the body of the request. This difference results in improved memory usage and can lead to faster transfers. It is easier to implement for clients as well.

Unlike method 1, this method does not buffer the full request before processing it. Data is written to the iRODS server as soon as it is received by the HTTP API server.

No adjustments to the Content-Type are necessary.

`irods-api-request-parallel-write-handle` and `irods-api-request-stream-index` only apply when writing to a replica in parallel. To obtain a parallel-write-handle, see [parallel_write_init](#parallel_write_init).

##### Notes

The behavior of the server is unspecified if a negative integer is passed as the replica number.

#### Response

If an HTTP status code of 200 is returned, the body of the response will contain the bytes read from the data object.

```
{
    "irods_response": {
        "status_code": 0,
        "status_message": "string" // Optional
    }
}
```

If there was an error, expect an HTTP status code in either the 4XX or 5XX range.

If the data object cannot be opened, the HTTP API will return an iRODS error code of -175000 (`INVALID_HANDLE`). This error code indicates that the data object may be locked.

### parallel_write_init

Initializes server-side state used for writing to a data object in parallel.

Returns a parallel-write-handle that can be used for parallel write operations.

#### Request

HTTP Method: POST

```bash
curl http://localhost:<port>/irods-http-api/<version>/data-objects \
    -H 'Authorization: Bearer <token>' \
    --data-urlencode 'op=parallel_write_init' \
    --data-urlencode 'lpath=<string>' \ # Absolute logical path to a data object.
    --data-urlencode 'resource=<string>' \ # The root resource to write to. Optional.
    --data-urlencode 'truncate=<integer>' \ # 0 or 1. Defaults to 1. Truncates the data object before writing. Optional.
    --data-urlencode 'append=<integer>' \ # 0 or 1. Defaults to 0. Appends the bytes to the data object. Optional.
    --data-urlencode 'ticket=<string>' \ # The ticket to enable for all streams. Optional.
    --data-urlencode 'stream-count=<integer>' # Number of streams to open.
```

#### Response

```
{
    "irods_response": {
        "status_code": 0,
        "status_message": "string" // Optional
    },
    "parallel_write_handle": "string" // Available only if the operation was successful.
}
```

If the data object cannot be opened, the HTTP API will return an iRODS error code of -175000 (`INVALID_HANDLE`). This error code indicates that the data object may be locked.

### parallel_write_shutdown

Instructs the server to shutdown and release any resources used for parallel write operations.

This operation MUST be called to complete the parallel write operation. Failing to call this operation will result in intermediate replicas and the server leaking memory.

#### Request

HTTP Method: POST

```bash
curl http://localhost:<port>/irods-http-api/<version>/data-objects \
    -H 'Authorization: Bearer <token>' \
    --data-urlencode 'op=parallel_write_shutdown' \
    --data-urlencode 'parallel-write-handle=<string>' # A handle obtained via the parallel_write_init operation.
```

#### Response

```
{
    "irods_response": {
        "status_code": 0,
        "status_message": "string" // Optional
    }
}
```

### modify_metadata

Adjust multiple AVUs on a data object.

#### Request

HTTP Method: POST

```bash
curl http://localhost:<port>/irods-http-api/<version>/data-objects \
    -H 'Authorization: Bearer <token>' \
    --data-urlencode 'op=modify_metadata' \
    --data-urlencode 'lpath=<string>' \ # Absolute logical path to a data object.
    --data-urlencode 'operations=<json_object>' \
    --data-urlencode 'admin=<integer>' # 0 or 1. Defaults to 0. Execute as a rodsadmin. Optional.
```

The JSON object passed to the `operations` parameter must have the following structure:

```js
[
    {
        "operation": "string", // add or remove.
        "attribute": "string",
        "value": "string",
        "units": "string" // Optional.
    },

    // Additional AVU operations ...
]
```

#### Response

If an HTTP status code of 200 is returned, the body of the response will contain JSON. Its structure is shown below.

```js
{
    "irods_response": {
        "status_code": 0
        "status_message": "string" // Optional
    }
}
```

If there was an error, expect an HTTP status code in either the 4XX or 5XX range. If an operation failed, the `irods_response` object will include an additional property called `failed_operation`. The structure is shown below.

```js
{
    "irods_response": {
        "status_code": 0
        "status_message": "string", // Optional
        "failed_operation": {
            "error_message": "string",
            "operation": {
                "operation": "string", // add or remove.
                "attribute": "string",
                "value": "string",
                "units": "string" // Optional.
            },
            "operation_index": 0
        }
    }
}
```

### set_permission

Sets the permission of a user or group on a data object.

#### Request

HTTP Method: POST

```bash
curl http://localhost:<port>/irods-http-api/<version>/data-objects \
    -H 'Authorization: Bearer <token>' \
    --data-urlencode 'op=set_permission' \
    --data-urlencode 'lpath=<string>' \ # Absolute logical path to a data object.
    --data-urlencode 'entity-name=<string>' \ # The name of a user or group.
    --data-urlencode 'permission=<string>' \
    --data-urlencode 'admin=<integer>' # 0 or 1. Defaults to 0. Execute as a rodsadmin. Optional.
```

The following permission levels are supported:
- own
- delete_object
- modify_object _(equivalent to write)_
- create_object
- delete_metadata
- modify_metadata
- create_metadata
- read_object _(equivalent to read)_
- read_metadata
- null

The following legacy permission levels are supported as well:
- own
- write
- read
- null

#### Response

If an HTTP status code of 200 is returned, the body of the response will contain JSON. Its structure is shown below.

```js
{
    "irods_response": {
        "status_code": 0
        "status_message": "string" // Optional
    }
}
```

If there was an error, expect an HTTP status code in either the 4XX or 5XX range.

### modify_permissions

Adjust permissions for multiple users and groups on a data object atomically.

#### Request

HTTP Method: POST

```bash
curl http://localhost:<port>/irods-http-api/<version>/data-objects \
    -H 'Authorization: Bearer <token>' \
    --data-urlencode 'op=modify_permissions' \
    --data-urlencode 'lpath=<string>' \ # Absolute logical path to a data object.
    --data-urlencode 'operations=<json_object>' \
    --data-urlencode 'admin=<integer>' # 0 or 1. Defaults to 0. Execute as a rodsadmin. Optional.
```

The JSON object passed to the `operations` parameter must have the following structure:

```js
[
    {
        "entity_name": "string", // The name of a user or group.
        "acl": "string" // null, read, write, or own.
    },

    // Additional ACL operations ...
]
```

#### Response

If an HTTP status code of 200 is returned, the body of the response will contain JSON. Its structure is shown below.

```js
{
    "irods_response": {
        "status_code": 0
        "status_message": "string" // Optional
    }
}
```

If there was an error, expect an HTTP status code in either the 4XX or 5XX range. If an operation failed, the `irods_response` object will include an additional property called `failed_operation`. The structure is shown below.

```js
{
    "irods_response": {
        "status_code": 0
        "status_message": "string", // Optional
        "failed_operation": {
            "error_message": "string",
            "operation": {
                "entity_name": "string", // The name of a user or group.
                "acl": "string" // null, read, write, or own.
            },
            "operation_index": 0
        }
    }
}
```

### modify_replica

Modifies properties of a single replica.

> [!WARNING]
> This operation requires rodsadmin level privileges and should only be used when there isn't a safer option. Misuse can lead to catalog inconsistencies and unexpected behavior.

#### Request

HTTP Method: POST

```bash
curl http://localhost:<port>/irods-http-api/<version>/data-objects \
    -H 'Authorization: Bearer <token>' \
    --data-urlencode 'op=modify_replica' \
    --data-urlencode 'lpath=<string>' \ # Absolute logical path to a data object.
    --data-urlencode 'resource-hierarchy=<string>' \
    --data-urlencode 'replica-number=<integer>' \
    --data-urlencode 'new-data-checksum=<string>' \
    --data-urlencode 'new-data-comments=<string>' \
    --data-urlencode 'new-data-create-time=<integer>' \
    --data-urlencode 'new-data-expiry=<integer>' \
    --data-urlencode 'new-data-mode=<string>' \
    --data-urlencode 'new-data-modify-time=<string>' \
    --data-urlencode 'new-data-path=<string>' \
    --data-urlencode 'new-data-replica-number=<integer>' \
    --data-urlencode 'new-data-replica-status=<integer>' \
    --data-urlencode 'new-data-resource-id=<integer>' \
    --data-urlencode 'new-data-size=<integer>' \
    --data-urlencode 'new-data-status=<string>' \
    --data-urlencode 'new-data-type-name=<string>' \
    --data-urlencode 'new-data-version=<string>' \
    --data-urlencode 'new-data-access-time=<string>' # Must be 11 bytes in length and padded with leading zeros.
```

`resource-hierarchy` and `replica-number` are mutually exclusive parameters.

`new-data-access-time` is supported by iRODS 5 only. Attempting to modify this property while connected to an iRODS server earlier than 5.0.0 will result in an HTTP status code of 400.

All parameters having a prefix of `new-` represent modifiable properties of the target replica. At least one modifiable property is required for the operation to succeed. This operation allows multiple properties to be modified in a single call.

#### Response

If an HTTP status code of 200 is returned, the body of the response will contain JSON. Its structure is shown below.

```js
{
    "irods_response": {
        "status_code": 0
        "status_message": "string" // Optional
    }
}
```

If there was an error, expect an HTTP status code in either the 4XX or 5XX range.

## Information Operations

Returns general information about the iRODS HTTP API server.

### Request

HTTP Method: GET

```bash
curl http://localhost:<port>/irods-http-api/<version>/info
```

### Response

If an HTTP status code of 200 is returned, the body of the response will contain JSON. Its structure is shown below.

```js
{
    "api_version": "string",
    "build": "string",
    "irods_zone": "string",
    "max_number_of_parallel_write_streams": 0,
    "max_number_of_rows_per_catalog_query": 0,
    "max_size_of_request_body_in_bytes": 0,
    "openid_connect_enabled": false
}
```

## Query Operations

### execute_genquery

Executes a GenQuery string and returns the results.

#### Request

HTTP Method: GET

```bash
curl http://localhost:<port>/irods-http-api/<version>/query \
    -H 'Authorization: Bearer <token>' \
    --data-urlencode 'op=execute_genquery' \
    --data-urlencode 'query=<string>' \
    --data-urlencode 'offset=<integer>' \ # Number of rows to skip. Defaults to 0. Optional.
    --data-urlencode 'count=<integer>' \ # Number of rows to return. Default set by administrator. Optional.
    --data-urlencode 'case-sensitive=<integer>' \ # Execute a case sensitive/insensitive query. Defaults to 1. Only supported by GenQuery1. Optional.
    --data-urlencode 'distinct=<integer>' \ # Collapse duplicate rows. Defaults to 1. Only supported by GenQuery1. Optional.
    --data-urlencode 'parser=<string>' \ # genquery1 or genquery2. Defaults to genquery1. Optional.
    --data-urlencode 'sql-only=<integer>' \ # 0 or 1. Defaults to 0. Only supported by GenQuery2. Optional.
    --data-urlencode 'zone=<string>' \ # The zone name. Defaults to the local zone. Optional.
    -G
```

`count` will be clamped to the range [1, _N_] where _N_ represents the max number of rows that can be returned by any query. The max number of rows is defined by the administrator of the iRODS HTTP API and can be obtained by sending an HTTP GET request to the /info endpoint. See [Information Operations](#information-operations) for more details.

#### Response

If an HTTP status code of 200 is returned, the body of the response will contain JSON. Its structure is shown below.

```js
{
    "irods_response": {
        "status_code": 0
        "status_message": "string" // Optional
    },
    "rows": [
        ["string", "string", "string"],

        // Additional rows ...
    ],
    "sql": "string" // If "sql-only" option is set to 1.
}
```

If there was an error, expect an HTTP status code in either the 4XX or 5XX range.

### execute_specific_query

Executes a specific query and returns the results.

#### Request

HTTP Method: GET

```bash
curl http://localhost:<port>/irods-http-api/<version>/query \
    -H 'Authorization: Bearer <token>' \
    --data-urlencode 'op=execute_specific_query' \
    --data-urlencode 'name=<string>' \ # Name of specific query.
    --data-urlencode 'args=<string>' \ # List of arguments. Optional.
    --data-urlencode 'args-delimiter=<string>' \ # Delimiter used to separate arguments. Defaults to comma (,). Optional.
    --data-urlencode 'offset=<integer>' \ # Number of rows to skip. Defaults to 0. Optional.
    --data-urlencode 'count=<integer>' \ # Number of rows to return. Default set by administrator. Optional.
    -G
```

`count` will be clamped to the range [1, _N_] where _N_ represents the max number of rows that can be returned by any query. The max number of rows is defined by the administrator of the iRODS HTTP API and can be obtained by sending an HTTP GET request to the /info endpoint. See [Information Operations](#information-operations) for more details.

```js
{
    "irods_response": {
        "status_code": 0
        "status_message": "string" // Optional
    },
    "rows": [
        ["string", "string", "string"],

        // Additional rows ...
    ]
}
```

### add_specific_query

Adds a SpecificQuery to the iRODS zone.

This operation requires rodsadmin level privileges.

#### Request

HTTP Method: POST

```bash
curl http://localhost:<port>/irods-http-api/<version>/query \
    -H 'Authorization: Bearer <token>' \
    --data-urlencode 'op=add_specific_query' \
    --data-urlencode 'name=<string>' \ # The name of the SpecificQuery.
    --data-urlencode 'sql=<string>' # The SQL attached to the SpecificQuery.
```

#### Response

If an HTTP status code of 200 is returned, the body of the response will contain JSON. Its structure is shown below.

```js
{
    "irods_response": {
        "status_code": 0
        "status_message": "string" // Optional
    }
}
```

### remove_specific_query

Removes a SpecificQuery from the iRODS zone.

This operation requires rodsadmin level privileges.

#### Request

HTTP Method: POST

```bash
curl http://localhost:<port>/irods-http-api/<version>/query \
    -H 'Authorization: Bearer <token>' \
    --data-urlencode 'op=remove_specific_query' \
    --data-urlencode 'name=<string>' # The name of the SpecificQuery.
```

#### Response

If an HTTP status code of 200 is returned, the body of the response will contain JSON. Its structure is shown below.

```js
{
    "irods_response": {
        "status_code": 0
        "status_message": "string" // Optional
    }
}
```

## Resource Operations

### create

Creates a new resource.

This operation requires rodsadmin level privileges.

#### Request

HTTP Method: POST

```bash
curl http://localhost:<port>/irods-http-api/<version>/resources \
    -H 'Authorization: Bearer <token>' \
    --data-urlencode 'op=create' \
    --data-urlencode 'name=<string>' \ # Name of the resource.
    --data-urlencode 'type=<string>' \ # Type of the resource.
    --data-urlencode 'host=<string>' \ # Depends on the resource's type. May be required.
    --data-urlencode 'vault-path=<string>' \ # Depends on the resource's type. May be required.
    --data-urlencode 'context=<string>' # Depends on the resource's type. May be required.
```

#### Response

If an HTTP status code of 200 is returned, the body of the response will contain JSON. Its structure is shown below.

```js
{
    "irods_response": {
        "status_code": 0
        "status_message": "string" // Optional
    }
}
```

If there was an error, expect an HTTP status code in either the 4XX or 5XX range.

### remove

Removes a resource.

This operation requires rodsadmin level privileges.

#### Request

HTTP Method: POST

```bash
curl http://localhost:<port>/irods-http-api/<version>/resources \
    -H 'Authorization: Bearer <token>' \
    --data-urlencode 'op=remove' \
    --data-urlencode 'name=<string>' # Name of the resource.
```

#### Response

If an HTTP status code of 200 is returned, the body of the response will contain JSON. Its structure is shown below.

```js
{
    "irods_response": {
        "status_code": 0
        "status_message": "string" // Optional
    }
}
```

If there was an error, expect an HTTP status code in either the 4XX or 5XX range.

### modify

Modifies a single property of a resource.

This operation requires rodsadmin level privileges.

#### Request

HTTP Method: POST

```bash
curl http://localhost:<port>/irods-http-api/<version>/resources \
    -H 'Authorization: Bearer <token>' \
    --data-urlencode 'op=modify' \
    --data-urlencode 'name=<string>' \ # Name of the resource.
    --data-urlencode 'property=<string>' \ # Name of the property to modify.
    --data-urlencode 'value=<string>' # The new value of the property.
```

The following properties are supported:
- name
- type
- host
- vault_path
- context
- status
- free_space
- comments
- information

For the `status` property, `value` must be set to `up` or `down`. Attempting to pass a value other the ones described will result in an HTTP error.

#### Response

If an HTTP status code of 200 is returned, the body of the response will contain JSON. Its structure is shown below.

```js
{
    "irods_response": {
        "status_code": 0
        "status_message": "string" // Optional
    }
}
```

If there was an error, expect an HTTP status code in either the 4XX or 5XX range.

### add_child

Creates a parent-child relationship between two resources.

This operation requires rodsadmin level privileges.

#### Request

HTTP Method: POST

```bash
curl http://localhost:<port>/irods-http-api/<version>/resources \
    -H 'Authorization: Bearer <token>' \
    --data-urlencode 'op=add_child' \
    --data-urlencode 'parent-name=<string>' \
    --data-urlencode 'child-name=<string>' \
    --data-urlencode 'context=<string>' # Additional information for the zone. Optional.
```

#### Response

If an HTTP status code of 200 is returned, the body of the response will contain JSON. Its structure is shown below.

```js
{
    "irods_response": {
        "status_code": 0
        "status_message": "string" // Optional
    }
}
```

If there was an error, expect an HTTP status code in either the 4XX or 5XX range.

### remove_child

Removes the parent-child relationship between two resources.

This operation requires rodsadmin level privileges.

#### Request

HTTP Method: POST

```bash
curl http://localhost:<port>/irods-http-api/<version>/resources \
    -H 'Authorization: Bearer <token>' \
    --data-urlencode 'op=remove_child' \
    --data-urlencode 'parent-name=<string>' \
    --data-urlencode 'child-name=<string>'
```

#### Response

If an HTTP status code of 200 is returned, the body of the response will contain JSON. Its structure is shown below.

```js
{
    "irods_response": {
        "status_code": 0
        "status_message": "string" // Optional
    }
}
```

If there was an error, expect an HTTP status code in either the 4XX or 5XX range.

### rebalance

Rebalances a resource hierarchy.

This operation requires rodsadmin level privileges.

#### Request

HTTP Method: POST

```bash
curl http://localhost:<port>/irods-http-api/<version>/resources \
    -H 'Authorization: Bearer <token>' \
    --data-urlencode 'op=rebalance' \
    --data-urlencode 'name=<string>'
```

#### Response

If an HTTP status code of 200 is returned, the body of the response will contain JSON. Its structure is shown below.

```js
{
    "irods_response": {
        "status_code": 0
        "status_message": "string" // Optional
    }
}
```

If there was an error, expect an HTTP status code in either the 4XX or 5XX range.

### stat

Returns information about a resource.

#### Request

HTTP Method: GET

```bash
curl http://localhost:<port>/irods-http-api/<version>/resources \
    -H 'Authorization: Bearer <token>' \
    --data-urlencode 'op=stat' \
    --data-urlencode 'name=<string>' \
    -G
```

#### Response

If an HTTP status code of 200 is returned, the body of the response will contain JSON. Its structure is shown below.

```js
{
    "irods_response": {
        "status_code": 0
        "status_message": "string" // Optional
    },
    "exists": false,
    "info": {
        "id": "string",
        "name": "string",
        "type": "string",
        "zone": "string",
        "host": "string",
        "vault_path": "string",
        "status": "string",
        "context": "string",
        "comments": "string",
        "information": "string",
        "free_space": "string",
        "free_space_last_modified": 0,
        "parent_id": "string",
        "created": 0,
        "last_modified": 0,
        "last_modified_millis": 0
    }
}
```

If there was an error, expect an HTTP status code in either the 4XX or 5XX range.

### modify_metadata

Adjust multiple AVUs on a resource.

This operation requires rodsadmin level privileges.

#### Request

HTTP Method: POST

```bash
curl http://localhost:<port>/irods-http-api/<version>/resources \
    -H 'Authorization: Bearer <token>' \
    --data-urlencode 'op=modify_metadata' \
    --data-urlencode 'name=<string>' \
    --data-urlencode 'operations=<json_object>'
```

The JSON object passed to the `operations` parameter must have the following structure:

```js
[
    {
        "operation": "string", // add or remove.
        "attribute": "string",
        "value": "string",
        "units": "string" // Optional.
    },

    // Additional AVU operations ...
]
```

#### Response

If an HTTP status code of 200 is returned, the body of the response will contain JSON. Its structure is shown below.

```js
{
    "irods_response": {
        "status_code": 0
        "status_message": "string" // Optional
    }
}
```

If there was an error, expect an HTTP status code in either the 4XX or 5XX range. If an operation failed, the `irods_response` object will include an additional property called `failed_operation`. The structure is shown below.

```js
{
    "irods_response": {
        "status_code": 0
        "status_message": "string", // Optional
        "failed_operation": {
            "error_message": "string",
            "operation": {
                "operation": "string", // add or remove.
                "attribute": "string",
                "value": "string",
                "units": "string" // Optional.
            },
            "operation_index": 0
        }
    }
}
```

## Rule Operations

### list_rule_engines

Lists the available rule engine plugin instances of the connected iRODS server.

#### Request

HTTP Method: GET

```bash
curl http://localhost:<port>/irods-http-api/<version>/rules \
    -H 'Authorization: Bearer <token>' \
    --data-urlencode 'op=list_rule_engines' \
    -G
```

#### Response

If an HTTP status code of 200 is returned, the body of the response will contain JSON. Its structure is shown below.

```js
{
    "irods_response": {
        "status_code": 0
        "status_message": "string" // Optional
    },
    "rule_engine_plugin_instances": [
        "string",
        "string",
        "string"
    ]
}
```

If there was an error, expect an HTTP status code in either the 4XX or 5XX range.

### execute

Executes rule code.

#### Request

HTTP Method: POST

```bash
curl http://localhost:<port>/irods-http-api/<version>/rules \
    -H 'Authorization: Bearer <token>' \
    --data-urlencode 'op=execute' \
    --data-urlencode 'rule-text=<string>' \ # The rule code to execute.
    --data-urlencode 'rep-instance=<string>' # The rule engine plugin to run the rule-text against. Optional.
```

If `rep-instance` is not passed, the rule text will be tried on ALL rule engine plugins. Any errors that occur will be ignored. Setting `rep-instance` is highly recommended.

#### Response

If an HTTP status code of 200 is returned, the body of the response will contain JSON. Its structure is shown below.

```js
{
    "irods_response": {
        "status_code": 0
        "status_message": "string" // Optional
    },
    "stdout": "string",
    "stderr": "string"
}
```

If there was an error, expect an HTTP status code in either the 4XX or 5XX range.

### remove_delay_rule

Removes a delay rule from the catalog.

#### Request

HTTP Method: POST

```bash
curl http://localhost:<port>/irods-http-api/<version>/rules \
    -H 'Authorization: Bearer <token>' \
    --data-urlencode 'op=remove_delay_rule' \
    --data-urlencode 'rule-id=<integer>' # The ID of delay rule to remove.
```

#### Response

If an HTTP status code of 200 is returned, the body of the response will contain JSON. Its structure is shown below.

```js
{
    "irods_response": {
        "status_code": 0
        "status_message": "string" // Optional
    }
}
```

If there was an error, expect an HTTP status code in either the 4XX or 5XX range.

## Ticket Operations

### create

Creates a new ticket for a collection or data object.

#### Request

HTTP Method: POST

```bash
curl http://localhost:<port>/irods-http-api/<version>/tickets \
    -H 'Authorization: Bearer <token>' \
    --data-urlencode 'op=create' \
    --data-urlencode 'lpath=<string>' \ # Absolute logical path to a data object or collection.
    --data-urlencode 'type=<string>' \ # read or write. Defaults to read. Optional.
    --data-urlencode 'use-count=<integer>' \ # Number of times the ticket can be used. Optional.
    --data-urlencode 'write-data-object-count=<integer>' \ # Max number of writes that can be performed. Optional.
    --data-urlencode 'write-byte-count=<integer>' \ # Max number of bytes that can be written. Optional.
    --data-urlencode 'seconds-until-expiration=<integer>' \ # Number of seconds before the ticket expires. Optional.
    --data-urlencode 'users=<string>' \ # Comma-delimited list of users allowed to use the ticket. Optional.
    --data-urlencode 'groups=<string>' \ # Comma-delimited list of groups allowed to use the ticket. Optional.
    --data-urlencode 'hosts=<string>' # Comma-delimited list of hosts allowed to use the ticket. Optional.
```

#### Response

If an HTTP status code of 200 is returned, the body of the response will contain JSON. Its structure is shown below.

```js
{
    "irods_response": {
        "status_code": 0
        "status_message": "string" // Optional
    },
    "ticket": "string" // The generated ticket string.
}
```

If there was an error, expect an HTTP status code in either the 4XX or 5XX range.

### remove

Removes a ticket.

#### Request

HTTP Method: POST

```bash
curl http://localhost:<port>/irods-http-api/<version>/tickets \
    -H 'Authorization: Bearer <token>' \
    --data-urlencode 'op=remove' \
    --data-urlencode 'name=<string>' # The ticket string.
```

#### Response

If an HTTP status code of 200 is returned, the body of the response will contain JSON. Its structure is shown below.

```js
{
    "irods_response": {
        "status_code": 0
        "status_message": "string" // Optional
    }
}
```

If there was an error, expect an HTTP status code in either the 4XX or 5XX range.

## User and Group Operations

### create_user

Creates a new user.

This operation requires rodsadmin or groupadmin level privileges.

#### Request

HTTP Method: POST

```bash
curl http://localhost:<port>/irods-http-api/<version>/users-groups \
    -H 'Authorization: Bearer <token>' \
    --data-urlencode 'op=create_user' \
    --data-urlencode 'name=<string>' \ # Name of user.
    --data-urlencode 'zone=<string>' \ # Name of zone for user.
    --data-urlencode 'user-type=<string>' # rodsuser, groupadmin, or rodsadmin. Defaults to rodsuser. Optional.
```

#### Response

If an HTTP status code of 200 is returned, the body of the response will contain JSON. Its structure is shown below.

```js
{
    "irods_response": {
        "status_code": 0
        "status_message": "string" // Optional
    }
}
```

If there was an error, expect an HTTP status code in either the 4XX or 5XX range.

### remove_user

Removes a user.

This operation requires rodsadmin level privileges.

#### Request

HTTP Method: POST

```bash
curl http://localhost:<port>/irods-http-api/<version>/users-groups \
    -H 'Authorization: Bearer <token>' \
    --data-urlencode 'op=remove_user' \
    --data-urlencode 'name=<string>' \ # Name of user.
    --data-urlencode 'zone=<string>' # Name of zone for user.
```

#### Response

If an HTTP status code of 200 is returned, the body of the response will contain JSON. Its structure is shown below.

```js
{
    "irods_response": {
        "status_code": 0
        "status_message": "string" // Optional
    }
}
```

If there was an error, expect an HTTP status code in either the 4XX or 5XX range.

### set_password

Changes a user's password.

This operation requires rodsadmin level privileges.

#### Request

HTTP Method: POST

```bash
curl http://localhost:<port>/irods-http-api/<version>/users-groups \
    -H 'Authorization: Bearer <token>' \
    --data-urlencode 'op=set_password' \
    --data-urlencode 'name=<string>' \ # Name of user.
    --data-urlencode 'zone=<string>' \ # Name of zone for user.
    --data-urlencode 'new-password=<string>'
```

#### Response

If an HTTP status code of 200 is returned, the body of the response will contain JSON. Its structure is shown below.

```js
{
    "irods_response": {
        "status_code": 0
        "status_message": "string" // Optional
    }
}
```

If there was an error, expect an HTTP status code in either the 4XX or 5XX range.

### set_user_type

Changes a user's type.

This operation requires rodsadmin level privileges.

#### Request

HTTP Method: POST

```bash
curl http://localhost:<port>/irods-http-api/<version>/users-groups \
    -H 'Authorization: Bearer <token>' \
    --data-urlencode 'op=set_user_type' \
    --data-urlencode 'name=<string>' \ # Name of user.
    --data-urlencode 'zone=<string>' \ # Name of zone for user.
    --data-urlencode 'new-user-type=<string>' # rodsuser, groupadmin, or rodsadmin.
```

#### Response

If an HTTP status code of 200 is returned, the body of the response will contain JSON. Its structure is shown below.

```js
{
    "irods_response": {
        "status_code": 0
        "status_message": "string" // Optional
    }
}
```

If there was an error, expect an HTTP status code in either the 4XX or 5XX range.

### create_group

Creates a new group.

This operation requires rodsadmin or groupadmin level privileges.

#### Request

HTTP Method: POST

```bash
curl http://localhost:<port>/irods-http-api/<version>/users-groups \
    -H 'Authorization: Bearer <token>' \
    --data-urlencode 'op=create_group' \
    --data-urlencode 'name=<string>' # Name of the group.
```

#### Response

If an HTTP status code of 200 is returned, the body of the response will contain JSON. Its structure is shown below.

```js
{
    "irods_response": {
        "status_code": 0
        "status_message": "string" // Optional
    }
}
```

If there was an error, expect an HTTP status code in either the 4XX or 5XX range.

### remove_group

Removes a group.

This operation requires rodsadmin level privileges.

#### Request

HTTP Method: POST

```bash
curl http://localhost:<port>/irods-http-api/<version>/users-groups \
    -H 'Authorization: Bearer <token>' \
    --data-urlencode 'op=remove_group' \
    --data-urlencode 'name=<string>' # Name of the group.
```

#### Response

If an HTTP status code of 200 is returned, the body of the response will contain JSON. Its structure is shown below.

```js
{
    "irods_response": {
        "status_code": 0
        "status_message": "string" // Optional
    }
}
```

If there was an error, expect an HTTP status code in either the 4XX or 5XX range.

### add_to_group

Adds a user to a group.

This operation requires rodsadmin or groupadmin level privileges.

Users of type groupadmin are allowed to execute this operation if at least one of the following conditions is true:
- The target group is initially empty
- The groupadmin user is a member of the group

Users of type groupadmin are always allowed to add themselves to an empty group. If the target group is not empty and the groupadmin user isn't a member of the group, execution of this operation will result in an error.

#### Request

HTTP Method: POST

```bash
curl http://localhost:<port>/irods-http-api/<version>/users-groups \
    -H 'Authorization: Bearer <token>' \
    --data-urlencode 'op=add_to_group' \
    --data-urlencode 'user=<string>' # Name of the user.
    --data-urlencode 'zone=<string>' # Name of zone for the user.
    --data-urlencode 'group=<string>' # Name of the group.
```

#### Response

If an HTTP status code of 200 is returned, the body of the response will contain JSON. Its structure is shown below.

```js
{
    "irods_response": {
        "status_code": 0
        "status_message": "string" // Optional
    }
}
```

If there was an error, expect an HTTP status code in either the 4XX or 5XX range.

### remove_from_group

Removes a user from a group.

This operation requires rodsadmin or groupadmin level privileges.

If the user is of type groupadmin, they must be a member of the target group to execute this operation.

#### Request

HTTP Method: POST

```bash
curl http://localhost:<port>/irods-http-api/<version>/users-groups \
    -H 'Authorization: Bearer <token>' \
    --data-urlencode 'op=remove_from_group' \
    --data-urlencode 'user=<string>' # Name of the user.
    --data-urlencode 'zone=<string>' # Name of zone for the user.
    --data-urlencode 'group=<string>' # Name of the group.
```

#### Response

If an HTTP status code of 200 is returned, the body of the response will contain JSON. Its structure is shown below.

```js
{
    "irods_response": {
        "status_code": 0
        "status_message": "string" // Optional
    }
}
```

If there was an error, expect an HTTP status code in either the 4XX or 5XX range.

### users

Lists all users in the zone.

This operation requires rodsadmin level privileges.

#### Request

HTTP Method: GET

```bash
curl http://localhost:<port>/irods-http-api/<version>/users-groups \
    -H 'Authorization: Bearer <token>' \
    --data-urlencode 'op=users' \
    -G
```

#### Response

If an HTTP status code of 200 is returned, the body of the response will contain JSON. Its structure is shown below.

```js
{
    "irods_response": {
        "status_code": 0
        "status_message": "string" // Optional
    },
    "users": [
        {
            "name": "string",
            "zone": "string"
        },

        // Additional users.
    ]
}
```

If there was an error, expect an HTTP status code in either the 4XX or 5XX range.

### groups

Lists all groups in the zone.

This operation requires rodsadmin level privileges.

#### Request

HTTP Method: GET

```bash
curl http://localhost:<port>/irods-http-api/<version>/users-groups \
    -H 'Authorization: Bearer <token>' \
    --data-urlencode 'op=groups' \
    -G
```

#### Response

If an HTTP status code of 200 is returned, the body of the response will contain JSON. Its structure is shown below.

```js
{
    "irods_response": {
        "status_code": 0
        "status_message": "string" // Optional
    },
    "groups": [
        "string",
        "string",
        "string"
    ]
}
```

If there was an error, expect an HTTP status code in either the 4XX or 5XX range.

### is_member_of_group

Returns whether a user is a member of a group.

#### Request

HTTP Method: GET

```bash
curl http://localhost:<port>/irods-http-api/<version>/users-groups \
    -H 'Authorization: Bearer <token>' \
    --data-urlencode 'op=is_member_of_group' \
    --data-urlencode 'group=<string>' \ # Name of the group.
    --data-urlencode 'user=<string>' \ # Name of the user.
    --data-urlencode 'zone=<string>' \ # Name of zone for the user.
    -G
```

#### Response

If an HTTP status code of 200 is returned, the body of the response will contain JSON. Its structure is shown below.

```js
{
    "irods_response": {
        "status_code": 0
        "status_message": "string" // Optional
    },
    "is_member": false
}
```

If there was an error, expect an HTTP status code in either the 4XX or 5XX range.

### stat

Returns information about a user or group.

#### Request

HTTP Method: GET

```bash
curl http://localhost:<port>/irods-http-api/<version>/users-groups \
    -H 'Authorization: Bearer <token>' \
    --data-urlencode 'op=stat' \
    --data-urlencode 'name=<string>' \ # Name of a user or group.
    --data-urlencode 'zone=<string>' \ # Name of zone if name represents a user. Not required for groups.
    -G
```

#### Response

If an HTTP status code of 200 is returned, the body of the response will contain JSON. Its structure is shown below.

```js
{
    "irods_response": {
        "status_code": 0
        "status_message": "string" // Optional
    },
    "exists": false,
    "id": "string",
    "local_unique_name": "string",
    "type": "string"
}
```

If there was an error, expect an HTTP status code in either the 4XX or 5XX range.

### modify_metadata

Adjust multiple AVUs on a user or group.

This operation requires rodsadmin level privileges.

#### Request

HTTP Method: POST

```bash
curl http://localhost:<port>/irods-http-api/<version>/users-groups \
    -H 'Authorization: Bearer <token>' \
    --data-urlencode 'op=modify_metadata' \
    --data-urlencode 'name=<string>' \
    --data-urlencode 'operations=<json_object>'
```

The JSON object passed to the `operations` parameter must have the following structure:

```js
[
    {
        "operation": "string", // add or remove.
        "attribute": "string",
        "value": "string",
        "units": "string" // Optional.
    },

    // Additional AVU operations ...
]
```

#### Response

If an HTTP status code of 200 is returned, the body of the response will contain JSON. Its structure is shown below.

```js
{
    "irods_response": {
        "status_code": 0
        "status_message": "string" // Optional
    }
}
```

If there was an error, expect an HTTP status code in either the 4XX or 5XX range. If an operation failed, the `irods_response` object will include an additional property called `failed_operation`. The structure is shown below.

```js
{
    "irods_response": {
        "status_code": 0
        "status_message": "string", // Optional
        "failed_operation": {
            "error_message": "string",
            "operation": {
                "operation": "string", // add or remove.
                "attribute": "string",
                "value": "string",
                "units": "string" // Optional.
            },
            "operation_index": 0
        }
    }
}
```

## Zone Operations

### add

Adds a remote zone to the local zone.

This operation requires rodsadmin level privileges.

#### Request

HTTP Method: POST

```bash
curl http://localhost:<port>/irods-http-api/<version>/zones \
    -H 'Authorization: Bearer <token>' \
    --data-urlencode 'op=add' \
    --data-urlencode 'name=<string>' \ # The name of the remote zone to add.
    --data-urlencode 'connection-info=<string>' \ # The host and port to connect to. Optional.
    --data-urlencode 'comment=<string>' # A comment to attach to the remote zone. Optional.
```

If `connection-info` is included, it must have the following structure: `<host>:<port>`

#### Response

If an HTTP status code of 200 is returned, the body of the response will contain JSON. Its structure is shown below.

```js
{
    "irods_response": {
        "status_code": 0
        "status_message": "string" // Optional
    }
}
```

If there was an error, expect an HTTP status code in either the 4XX or 5XX range.

### remove

Removes a remote zone from the local zone.

This operation requires rodsadmin level privileges.

#### Request

HTTP Method: POST

```bash
curl http://localhost:<port>/irods-http-api/<version>/zones \
    -H 'Authorization: Bearer <token>' \
    --data-urlencode 'op=remove' \
    --data-urlencode 'name=<string>' # The name of the remote zone to remove.
```

#### Response

If an HTTP status code of 200 is returned, the body of the response will contain JSON. Its structure is shown below.

```js
{
    "irods_response": {
        "status_code": 0
        "status_message": "string" // Optional
    }
}
```

If there was an error, expect an HTTP status code in either the 4XX or 5XX range.

### modify

Modifies properties of a remote zone.

This operation requires rodsadmin level privileges.

#### Request

HTTP Method: POST

```bash
curl http://localhost:<port>/irods-http-api/<version>/zones \
    -H 'Authorization: Bearer <token>' \
    --data-urlencode 'op=modify' \
    --data-urlencode 'name=<string>' \ # The name of the remote zone to modify.
    --data-urlencode 'property=<string>' \ # The property to modify.
    --data-urlencode 'value=<string>' # The new value of the property.
```

The following properties are supported:
- name
- connection_info
- comment

The value for `connection_info` must have the following structure: `<host>:<port>`

#### Response

If an HTTP status code of 200 is returned, the body of the response will contain JSON. Its structure is shown below.

```js
{
    "irods_response": {
        "status_code": 0
        "status_message": "string" // Optional
    }
}
```

If there was an error, expect an HTTP status code in either the 4XX or 5XX range.

### stat

Returns information about a zone.

#### Request

HTTP Method: GET

```bash
curl http://localhost:<port>/irods-http-api/<version>/zones \
    -H 'Authorization: Bearer <token>' \
    --data-urlencode 'op=stat' \
    --data-urlencode 'name=<string>' \
    -G
```

#### Response

If an HTTP status code of 200 is returned, the body of the response will contain JSON. Its structure is shown below.

```js
{
    "irods_response": {
        "status_code": 0
        "status_message": "string" // Optional
    },
    "exists": false,
    "info": {
        "id": 0,
        "name": "string",
        "type": "string",
        "connection_info": "string",
        "comment": "string"
    }
}
```

If there was an error, expect an HTTP status code in either the 4XX or 5XX range.

### report

Returns information about the iRODS zone.

This operation requires rodsadmin level privileges.

#### Request

HTTP Method: GET

```bash
curl http://localhost:<port>/irods-http-api/<version>/zones \
    -H 'Authorization: Bearer <token>' \
    --data-urlencode 'op=report' \
    -G
```

#### Response

If an HTTP status code of 200 is returned, the body of the response will contain JSON. Its structure is shown below.

```js
{
    "irods_response": {
        "status_code": 0
        "status_message": "string" // Optional
    },
    "zone_report": {
        // Equivalent output of executing izonereport.
    }
}
```

If there was an error, expect an HTTP status code in either the 4XX or 5XX range.
