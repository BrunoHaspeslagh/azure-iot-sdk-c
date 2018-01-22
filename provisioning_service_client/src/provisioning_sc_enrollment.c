// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license information.

#include <stdlib.h>  

#include "azure_c_shared_utility/xlogging.h"
#include "azure_c_shared_utility/gballoc.h"
#include "azure_c_shared_utility/map.h"

#include "provisioning_sc_enrollment.h"
#include "provisioning_sc_json_const.h"
#include "provisioning_sc_json.h"
#include "provisioning_sc_private_utility.h"
#include "provisioning_sc_twin.h"
#include "parson.h"

#define UNREFERENCED_PARAMETER(x) x

#define CERTIFICATE_TYPE_VALUES \
            CERTIFICATE_TYPE_NONE, \
            CERTIFICATE_TYPE_CLIENT, \
            CERTIFICATE_TYPE_SIGNING, \
            CERTIFICATE_TYPE_CA_REFERENCES \

//Note: CERTIFICATE_TYPE_NONE is invalid, indicating error
DEFINE_ENUM(CERTIFICATE_TYPE, CERTIFICATE_TYPE_VALUES);

typedef struct TPM_ATTESTATION_TAG
{
    char* endorsement_key;
    char* storage_root_key;
} TPM_ATTESTATION;

typedef struct X509_CERTIFICATE_INFO_TAG
{
    char* subject_name;
    char* sha1_thumbprint;
    char* sha256_thumbprint;
    char* issuer_name;
    char* not_before_utc;
    char* not_after_utc;
    char* serial_number;
    int version;
} X509_CERTIFICATE_INFO;

typedef struct X509_CERTIFICATE_WITH_INFO_TAG
{
    char* certificate;
    X509_CERTIFICATE_INFO* info;
} X509_CERTIFICATE_WITH_INFO;

typedef struct X509_CERTIFICATES_TAG
{
    X509_CERTIFICATE_WITH_INFO* primary;
    X509_CERTIFICATE_WITH_INFO* secondary;
} X509_CERTIFICATES;

typedef struct X509_CA_REFERENCES_TAG
{
    char* primary;
    char* secondary;
} X509_CA_REFERENCES;

typedef struct X509_ATTESTATION_TAG
{
    CERTIFICATE_TYPE type;
    union {
        X509_CERTIFICATES* client_certificates;
        X509_CERTIFICATES* signing_certificates;
        X509_CA_REFERENCES* ca_references;
    } certificates;

} X509_ATTESTATION;

typedef struct ATTESTATION_MECHANISM_TAG
{
    ATTESTATION_TYPE type;
    union {
        TPM_ATTESTATION* tpm;
        X509_ATTESTATION* x509;
    } attestation;
} ATTESTATION_MECHANISM;

typedef struct DEVICE_REGISTRATION_STATE_TAG
{
    char* registration_id;
    char* created_date_time_utc;
    char* device_id;
    REGISTRATION_STATUS status;
    char* updated_date_time_utc;
    int error_code;
    char* error_message;
    char* etag;
} DEVICE_REGISTRATION_STATE;

typedef struct INDIVIDUAL_ENROLLMENT_TAG
{
    char* registration_id; //read only
    char* device_id;
    DEVICE_REGISTRATION_STATE* registration_state; //read only
    ATTESTATION_MECHANISM* attestation_mechanism;
    INITIAL_TWIN_HANDLE initial_twin;
    char* etag;
    PROVISIONING_STATUS provisioning_status;
    char* created_date_time_utc; //read only
    char* updated_date_time_utc; //read only
} INDIVIDUAL_ENROLLMENT;

typedef struct ENROLLMENT_GROUP_TAG
{
    char* group_id; //read only
    ATTESTATION_MECHANISM* attestation_mechanism;
    INITIAL_TWIN_HANDLE initial_twin;
    char* etag;
    PROVISIONING_STATUS provisioning_status;
    char* created_date_time_utc; //read only
    char* updated_date_time_utc; //read only
} ENROLLMENT_GROUP;

DEFINE_ENUM_STRINGS(ATTESTATION_TYPE, ATTESTATION_TYPE_VALUES)
DEFINE_ENUM_STRINGS(CERTIFICATE_TYPE, CERTIFICATE_TYPE_VALUES)
DEFINE_ENUM_STRINGS(PROVISIONING_STATUS, PROVISIONING_STATUS_VALUES)
DEFINE_ENUM_STRINGS(REGISTRATION_STATUS, REGISTRATION_STATUS_VALUES)

static const REGISTRATION_STATUS registrationStatus_fromJson(const char* str_rep)
{
    REGISTRATION_STATUS new_status = REGISTRATION_STATUS_ERROR;

    if (str_rep != NULL)
    {
        if (strcmp(str_rep, REGISTRATION_STATUS_JSON_VALUE_UNASSIGNED) == 0)
        {
            new_status = REGISTRATION_STATUS_UNASSIGNED;
        }
        else if (strcmp(str_rep, REGISTRATION_STATUS_JSON_VALUE_ASSIGNING) == 0)
        {
            new_status = REGISTRATION_STATUS_ASSIGNING;
        }
        else if (strcmp(str_rep, REGISTRATION_STATUS_JSON_VALUE_ASSIGNED) == 0)
        {
            new_status = REGISTRATION_STATUS_ASSIGNED;
        }
        else if (strcmp(str_rep, REGISTRATION_STATUS_JSON_VALUE_FAILED) == 0)
        {
            new_status = REGISTRATION_STATUS_FAILED;
        }
        else if (strcmp(str_rep, REGISTRATION_STATUS_JSON_VALUE_DISABLED) == 0)
        {
            new_status = REGISTRATION_STATUS_DISABLED;
        }
        else
        {
            LogError("Could not convert '%s' from JSON", str_rep);
        }
    }

    return new_status;
}

static const char* provisioningStatus_toJson(const PROVISIONING_STATUS status)
{
    const char* result = NULL;
    if (status == PROVISIONING_STATUS_ENABLED)
    {
        result = PROVISIONING_STATUS_JSON_VALUE_ENABLED;
    }
    else if (status == PROVISIONING_STATUS_DISABLED)
    {
        result = PROVISIONING_STATUS_JSON_VALUE_DISABLED;
    }
    else
    {
        LogError("Could not convert '%s' to JSON", ENUM_TO_STRING(PROVISIONING_STATUS, status));
    }

    return result;
}

static const PROVISIONING_STATUS provisioningStatus_fromJson(const char* str_rep)
{
    PROVISIONING_STATUS new_status = PROVISIONING_STATUS_NONE;

    if (str_rep != NULL)
    {
        if (strcmp(str_rep, PROVISIONING_STATUS_JSON_VALUE_ENABLED) == 0)
        {
            new_status = PROVISIONING_STATUS_ENABLED;
        }
        else if (strcmp(str_rep, PROVISIONING_STATUS_JSON_VALUE_DISABLED) == 0)
        {
            new_status = PROVISIONING_STATUS_DISABLED;
        }
        else
        {
            LogError("Could not convert '%s' from JSON", str_rep);
        }
    }

    return new_status;
}

static const char* attestationType_toJson(const ATTESTATION_TYPE type)
{
    const char* result = NULL;
    if (type == ATTESTATION_TYPE_TPM)
    {
        result = ATTESTATION_TYPE_JSON_VALUE_TPM;
    }
    else if (type == ATTESTATION_TYPE_X509)
    {
        result = ATTESTATION_TYPE_JSON_VALUE_X509;
    }
    else
    {
        LogError("Could not convert '%s' to JSON", ENUM_TO_STRING(ATTESTATION_TYPE, type));
    }

    return result;
}

static const ATTESTATION_TYPE attestationType_fromJson(const char* str_rep)
{
    ATTESTATION_TYPE new_type = ATTESTATION_TYPE_NONE;

    if (str_rep != NULL)
    {
        if (strcmp(str_rep, ATTESTATION_TYPE_JSON_VALUE_TPM) == 0)
        {
            new_type = ATTESTATION_TYPE_TPM;
        }
        else if (strcmp(str_rep, ATTESTATION_TYPE_JSON_VALUE_X509) == 0)
        {
            new_type = ATTESTATION_TYPE_X509;
        }
        else
        {
            LogError("Could not convert '%s' from JSON", str_rep);
        }
    }

    return new_type;
}

static void x509CAReferences_free(X509_CA_REFERENCES* x509_ca_ref)
{
    if (x509_ca_ref != NULL)
    {
        free(x509_ca_ref->primary);
        free(x509_ca_ref->secondary);
        free(x509_ca_ref);
    }
}

static X509_CA_REFERENCES* x509CAReferences_create(const char* primary, const char* secondary)
{
    X509_CA_REFERENCES* new_x509CARef = NULL;

    if (primary == NULL)
    {
        LogError("Requires valid primary CA Reference");
    }
    else if ((new_x509CARef = malloc(sizeof(X509_CA_REFERENCES))) == NULL)
    {
        LogError("Failed to allocate memory for X509 CA References");
    }
    else
    {
        memset(new_x509CARef, 0, sizeof(X509_CA_REFERENCES));

        if (copy_string(&(new_x509CARef->primary), primary) != 0)
        {
            LogError("Error setting primary CA Reference in X509CAReferences");
            x509CAReferences_free(new_x509CARef);
            new_x509CARef = NULL;
        }
        else if (copy_string(&(new_x509CARef->secondary), secondary) != 0)
        {
            LogError("Error setting secondary CA Reference in X509CAReferences");
            x509CAReferences_free(new_x509CARef);
            new_x509CARef = NULL;
        }
    }

    return new_x509CARef;
}

static JSON_Value* x509CAReferences_toJson(const X509_CA_REFERENCES* x509_ca_ref)
{
    JSON_Value* root_value = NULL;
    JSON_Object* root_object = NULL;

    if (x509_ca_ref == NULL)
    {
        LogError("CA References is NULL");
    }
    else if ((root_value = json_value_init_object()) == NULL)
    {
        LogError("json_value_init_object failed");
    }
    else if ((root_object = json_value_get_object(root_value)) == NULL)
    {
        LogError("json_value_get_object failed");
        json_value_free(root_value);
        root_value = NULL;
    }

    else if (json_object_set_string(root_object, X509_CA_REFERENCES_JSON_KEY_PRIMARY, x509_ca_ref->primary) != JSONSuccess)
    {
        LogError("Failed to set '%s' in JSON String", X509_CA_REFERENCES_JSON_KEY_PRIMARY);
        json_value_free(root_value);
        root_value = NULL;
    }
    else if ((x509_ca_ref->secondary != NULL) && (json_object_set_string(root_object, X509_CA_REFERENCES_JSON_KEY_SECONDARY, x509_ca_ref->secondary) != JSONSuccess))
    {
        LogError("Failed to set '%s' in JSON String", X509_CA_REFERENCES_JSON_KEY_SECONDARY);
        json_value_free(root_value);
        root_value = NULL;
    }

    return root_value;
}

static X509_CA_REFERENCES* x509CAReferences_fromJson(JSON_Object* root_object)
{
    X509_CA_REFERENCES* new_x509CARef = NULL;

    if ((new_x509CARef = malloc(sizeof(X509_CA_REFERENCES))) == NULL)
    {
        LogError("Allocation of X509 CA References failed");
    }
    else
    {
        memset(new_x509CARef, 0, sizeof(X509_CA_REFERENCES));

        if (copy_json_string_field(&(new_x509CARef->primary), root_object, X509_CA_REFERENCES_JSON_KEY_PRIMARY) != 0)
        {
            LogError("Failed to set '%s' in X509 CA References", X509_CA_REFERENCES_JSON_KEY_PRIMARY);
            x509CAReferences_free(new_x509CARef);
            new_x509CARef = NULL;
        }
        else if (copy_json_string_field(&(new_x509CARef->secondary), root_object, X509_CA_REFERENCES_JSON_KEY_SECONDARY) != 0)
        {
            LogError("Failed to set '%s' in X509 CA References", X509_CA_REFERENCES_JSON_KEY_SECONDARY);
            x509CAReferences_free(new_x509CARef);
            new_x509CARef = NULL;
        }
    }

    return new_x509CARef;
}

static void x509CertificateInfo_free(X509_CERTIFICATE_INFO* x509_info)
{
    if (x509_info != NULL)
    {
        free(x509_info->subject_name);
        free(x509_info->sha1_thumbprint);
        free(x509_info->sha256_thumbprint);
        free(x509_info->issuer_name);
        free(x509_info->not_before_utc);
        free(x509_info->not_after_utc);
        free(x509_info->serial_number);
        free(x509_info);
    }
}

static JSON_Value* x509CertificateInfo_toJson(const X509_CERTIFICATE_INFO* x509_info)
{
    JSON_Value* root_value = NULL;
    JSON_Object* root_object = NULL;

    //Setup
    if ((root_value = json_value_init_object()) == NULL)
    {
        LogError("json_value_init_object failed");
    }
    else if ((root_object = json_value_get_object(root_value)) == NULL)
    {
        LogError("json_value_get_object failed");
        json_value_free(root_value);
        root_value = NULL;
    }

    //Set data
    else if (json_object_set_string(root_object, X509_CERTIFICATE_INFO_JSON_KEY_SUBJECT_NAME, x509_info->subject_name) != JSONSuccess)
    {
        LogError("Failed to set '%s' in JSON string", X509_CERTIFICATE_INFO_JSON_KEY_SUBJECT_NAME);
        json_value_free(root_value);
        root_value = NULL;
    }
    else if (json_object_set_string(root_object, X509_CERTIFICATE_INFO_JSON_KEY_SHA1, x509_info->sha1_thumbprint) != JSONSuccess)
    {
        LogError("Failed to set '%s' in JSON string", X509_CERTIFICATE_INFO_JSON_KEY_SHA1);
        json_value_free(root_value);
        root_value = NULL;
    }
    else if (json_object_set_string(root_object, X509_CERTIFICATE_INFO_JSON_KEY_SHA256, x509_info->sha256_thumbprint) != JSONSuccess)
    {
        LogError("Failed to set '%s' in JSON string", X509_CERTIFICATE_INFO_JSON_KEY_SHA256);
        json_value_free(root_value);
        root_value = NULL;
    }
    else if (json_object_set_string(root_object, X509_CERTIFICATE_INFO_JSON_KEY_ISSUER, x509_info->issuer_name) != JSONSuccess)
    {
        LogError("Failed to set '%s' in JSON string", X509_CERTIFICATE_INFO_JSON_KEY_ISSUER);
        json_value_free(root_value);
        root_value = NULL;
    }
    else if (json_object_set_string(root_object, X509_CERTIFICATE_INFO_JSON_KEY_NOT_BEFORE, x509_info->not_before_utc) != JSONSuccess)
    {
        LogError("Failed to set '%s' in JSON string", X509_CERTIFICATE_INFO_JSON_KEY_NOT_BEFORE);
        json_value_free(root_value);
        root_value = NULL;
    }
    else if (json_object_set_string(root_object, X509_CERTIFICATE_INFO_JSON_KEY_NOT_AFTER, x509_info->not_after_utc) != JSONSuccess)
    {
        LogError("Failed to set '%s' in JSON string", X509_CERTIFICATE_INFO_JSON_KEY_NOT_AFTER);
        json_value_free(root_value);
        root_value = NULL;
    }
    else if (json_object_set_string(root_object, X509_CERTIFICATE_INFO_JSON_KEY_SERIAL_NO, x509_info->serial_number) != JSONSuccess)
    {
        LogError("Failed to set '%s' in JSON string", X509_CERTIFICATE_INFO_JSON_KEY_SERIAL_NO);
        json_value_free(root_value);
        root_value = NULL;
    }
    else if (json_object_set_number(root_object, X509_CERTIFICATE_INFO_JSON_KEY_VERSION, x509_info->version) != JSONSuccess)
    {
        LogError("Failed to set '%s' in JSON string", X509_CERTIFICATE_INFO_JSON_KEY_VERSION);
        json_value_free(root_value);
        root_value = NULL;
    }

    return root_value;
}

static X509_CERTIFICATE_INFO* x509CertificateInfo_fromJson(JSON_Object* root_object)
{
    X509_CERTIFICATE_INFO* new_x509Info = NULL;

    if (root_object == NULL)
    {
        LogError("No X509 Certificate Info in JSON");
    }
    else if ((new_x509Info = malloc(sizeof(X509_CERTIFICATE_INFO))) == NULL)
    {
        LogError("Allocation of X509 Certificate Info failed");
    }
    else
    {
        memset(new_x509Info, 0, sizeof(X509_CERTIFICATE_INFO));

        if (copy_json_string_field(&(new_x509Info->subject_name), root_object, X509_CERTIFICATE_INFO_JSON_KEY_SUBJECT_NAME) != 0)
        {
            LogError("Failed to set '%s' in X509 Certificate Info", X509_CERTIFICATE_INFO_JSON_KEY_SUBJECT_NAME);
            x509CertificateInfo_free(new_x509Info);
            new_x509Info = NULL;
        }
        else if (copy_json_string_field(&(new_x509Info->sha1_thumbprint), root_object, X509_CERTIFICATE_INFO_JSON_KEY_SHA1) != 0)
        {
            LogError("Failed to set '%s' in X509 Certificate Info", X509_CERTIFICATE_INFO_JSON_KEY_SHA1);
            x509CertificateInfo_free(new_x509Info);
            new_x509Info = NULL;
        }
        else if (copy_json_string_field(&(new_x509Info->sha256_thumbprint), root_object, X509_CERTIFICATE_INFO_JSON_KEY_SHA256) != 0)
        {
            LogError("Failed to set '%s' in X509 Certificate Info", X509_CERTIFICATE_INFO_JSON_KEY_SHA256);
            x509CertificateInfo_free(new_x509Info);
            new_x509Info = NULL;
        }
        else if (copy_json_string_field(&(new_x509Info->issuer_name), root_object, X509_CERTIFICATE_INFO_JSON_KEY_ISSUER) != 0)
        {
            LogError("Failed to set '%s' in X509 Certificate Info", X509_CERTIFICATE_INFO_JSON_KEY_ISSUER);
            x509CertificateInfo_free(new_x509Info);
            new_x509Info = NULL;
        }
        else if (copy_json_string_field(&(new_x509Info->not_before_utc), root_object, X509_CERTIFICATE_INFO_JSON_KEY_NOT_BEFORE) != 0)
        {
            LogError("Failed to set '%s' in X509 Certificate Info", X509_CERTIFICATE_INFO_JSON_KEY_NOT_BEFORE);
            x509CertificateInfo_free(new_x509Info);
            new_x509Info = NULL;
        }
        else if (copy_json_string_field(&(new_x509Info->not_after_utc), root_object, X509_CERTIFICATE_INFO_JSON_KEY_NOT_AFTER) != 0)
        {
            LogError("Failed to set '%s' in X509 Certificate Info", X509_CERTIFICATE_INFO_JSON_KEY_NOT_AFTER);
            x509CertificateInfo_free(new_x509Info);
            new_x509Info = NULL;
        }
        else if (copy_json_string_field(&(new_x509Info->serial_number), root_object, X509_CERTIFICATE_INFO_JSON_KEY_SERIAL_NO) != 0)
        {
            LogError("Failed to set '%s' in X509 Certificate Info", X509_CERTIFICATE_INFO_JSON_KEY_SERIAL_NO);
            x509CertificateInfo_free(new_x509Info);
            new_x509Info = NULL;
        }
        else
        {
            new_x509Info->version = (int)json_object_get_number(root_object, X509_CERTIFICATE_INFO_JSON_KEY_VERSION);
        }
    }

    return new_x509Info;
}

static JSON_Value* x509CertificateWithInfo_toJson(const X509_CERTIFICATE_WITH_INFO* x509_certinfo)
{
    JSON_Value* root_value = NULL;
    JSON_Object* root_object = NULL;

    //Setup
    if ((root_value = json_value_init_object()) == NULL)
    {
        LogError("json_value_init_object failed");
    }
    else if ((root_object = json_value_get_object(root_value)) == NULL)
    {
        LogError("json_value_get_object failed");
        json_value_free(root_value);
        root_value = NULL;
    }

    //Set data
    else if ((x509_certinfo->certificate != NULL) && (json_object_set_string(root_object, X509_CERTIFICATE_WITH_INFO_JSON_KEY_CERTIFICATE, x509_certinfo->certificate) != JSONSuccess))
    {
        LogError("Failed to set '%s' in JSON string representation of X509 Certificate With Info", X509_CERTIFICATE_WITH_INFO_JSON_KEY_CERTIFICATE);
        json_value_free(root_value);
        root_value = NULL;
    }
    else if (x509_certinfo->info != NULL)
    {
        if (json_serialize_and_set_struct(root_object, X509_CERTIFICATE_WITH_INFO_JSON_KEY_INFO, x509_certinfo->info, x509CertificateInfo_toJson, REQUIRED) != 0)
        {
            LogError("Failed to set '%s' in JSON string representation of X509 Certificate With Info", X509_CERTIFICATE_WITH_INFO_JSON_KEY_INFO);
            json_value_free(root_value);
            root_value = NULL;
        }
    }


    return root_value;
}

static void x509CertificateWithInfo_free(X509_CERTIFICATE_WITH_INFO* x509_certinfo)
{
    if (x509_certinfo != NULL)
    {
        free(x509_certinfo->certificate);
        x509CertificateInfo_free(x509_certinfo->info);
        free(x509_certinfo);
    }
}

static X509_CERTIFICATE_WITH_INFO* x509CertificateWithInfo_fromJson(JSON_Object* root_object)
{
    X509_CERTIFICATE_WITH_INFO* new_x509CertInfo = NULL;

    if (root_object == NULL)
    {
        LogError("No Certificate with Info in JSON");
    }
    else if ((new_x509CertInfo = malloc(sizeof(X509_CERTIFICATE_WITH_INFO))) == NULL)
    {
        LogError("Allocation of X509 Certificate With Info failed");
    }
    else
    {
        memset(new_x509CertInfo, 0, sizeof(X509_CERTIFICATE_WITH_INFO));
        if (copy_json_string_field(&(new_x509CertInfo->certificate), root_object, X509_CERTIFICATE_WITH_INFO_JSON_KEY_CERTIFICATE) != 0)
        {
            LogError("Failed to set '%s' in X509 Certificate With Info", X509_CERTIFICATE_WITH_INFO_JSON_KEY_CERTIFICATE);
            x509CertificateWithInfo_free(new_x509CertInfo);
            new_x509CertInfo = NULL;
        }
        else if (json_deserialize_and_get_struct(&(new_x509CertInfo->info), root_object, X509_CERTIFICATE_WITH_INFO_JSON_KEY_INFO, x509CertificateInfo_fromJson, REQUIRED) != 0)
        {
            LogError("Failed to set '%s' in X509 Certificate With Info", X509_CERTIFICATE_WITH_INFO_JSON_KEY_INFO);
            x509CertificateWithInfo_free(new_x509CertInfo);
            new_x509CertInfo = NULL;
        }
    }

    return new_x509CertInfo;
}

static X509_CERTIFICATE_WITH_INFO* x509CertificateWithInfo_create(const char* cert)
{
    X509_CERTIFICATE_WITH_INFO* new_x509CertWithInfo = NULL;

    if (cert == NULL)
    {
        LogError("certificate is NULL");
    }
    else if ((new_x509CertWithInfo = malloc(sizeof(X509_CERTIFICATE_WITH_INFO))) == NULL)
    {
        LogError("Allocating memory for X509 Certificate With Info failed");
    }
    else
    {
        memset(new_x509CertWithInfo, 0, sizeof(X509_CERTIFICATE_WITH_INFO));

        if (copy_string(&(new_x509CertWithInfo->certificate), cert) != 0)
        {
            LogError("Error setting certificate in X509CertificateWithInfo");
            x509CertificateWithInfo_free(new_x509CertWithInfo);
            new_x509CertWithInfo = NULL;
        }
        //adding the actual info struct is only when building from JSON
    }

    return new_x509CertWithInfo;
}

static void x509Certificates_free(X509_CERTIFICATES* x509_certs)
{
    if (x509_certs != NULL)
    {
        if (x509_certs->primary != NULL)
        {
            x509CertificateWithInfo_free(x509_certs->primary);
        }
        if (x509_certs->secondary != NULL)
        {
            x509CertificateWithInfo_free(x509_certs->secondary);
        }
        free(x509_certs);
    }
}

static X509_CERTIFICATES* x509Certificates_create(const char* primary_cert, const char* secondary_cert)
{
    X509_CERTIFICATES* new_x509Certs = NULL;

    if (primary_cert == NULL)
    {
        LogError("Requires valid primary certificate");
    }
    else if ((new_x509Certs = malloc(sizeof(X509_CERTIFICATES))) == NULL)
    {
        LogError("Failed to allocate memory for X509 Certificates");
    }
    else
    {
        memset(new_x509Certs, 0, sizeof(X509_CERTIFICATES));

        //Primary Cert is mandatory
        if ((new_x509Certs->primary = x509CertificateWithInfo_create(primary_cert)) == NULL)
        {
            LogError("Failed to create Primary Certificate");
            x509Certificates_free(new_x509Certs);
            new_x509Certs = NULL;
        }

        //Secondary Cert is optional
        else if ((secondary_cert != NULL) && ((new_x509Certs->secondary = x509CertificateWithInfo_create(secondary_cert)) == NULL))
        {
            LogError("Failed to create Secondary Certificate");
            x509Certificates_free(new_x509Certs);
            new_x509Certs = NULL;
        }
    }

    return new_x509Certs;
}

static JSON_Value* x509Certificates_toJson(const X509_CERTIFICATES* x509_certs)
{
    JSON_Value* root_value = NULL;
    JSON_Object* root_object = NULL;

    //Setup
    if ((root_value = json_value_init_object()) == NULL)
    {
        LogError("json_value_init_object failed");
    }
    else if ((root_object = json_value_get_object(root_value)) == NULL)
    {
        LogError("json_value_get_object failed");
        json_value_free(root_value);
        root_value = NULL;
    }

    //Set data
    else
    {
        if (json_serialize_and_set_struct(root_object, X509_CERTIFICATES_JSON_KEY_PRIMARY, x509_certs->primary, x509CertificateWithInfo_toJson, REQUIRED) != 0)
        {
            LogError("Failed to set '%s' in JSON string representation of X509 Certificates", X509_CERTIFICATES_JSON_KEY_PRIMARY);
            json_value_free(root_value);
            root_value = NULL;
        }
        else if (json_serialize_and_set_struct(root_object, X509_CERTIFICATES_JSON_KEY_SECONDARY, x509_certs->secondary, x509CertificateWithInfo_toJson, OPTIONAL) != 0)
        {
            LogError("Failed to set '%s' in JSON string representation of X509 Certificates", X509_CERTIFICATES_JSON_KEY_SECONDARY);
            json_value_free(root_value);
            root_value = NULL;
        }
    }

    return root_value;
}

static X509_CERTIFICATES* x509Certificates_fromJson(JSON_Object* root_object)
{
    X509_CERTIFICATES* new_x509certs = NULL;

    if (root_object == NULL)
    {
        LogError("No X509 Certificates in JSON");
    }
    else if ((new_x509certs = malloc(sizeof(X509_CERTIFICATES))) == NULL)
    {
        LogError("Allocation of X509 Certificates failed");
    }
    else
    {
        memset(new_x509certs, 0, sizeof(X509_CERTIFICATES));

        if (json_deserialize_and_get_struct(&(new_x509certs->primary), root_object, X509_CERTIFICATES_JSON_KEY_PRIMARY, x509CertificateWithInfo_fromJson, REQUIRED) != 0)
        {
            LogError("Failed to set '%s' in X509 Certificates", X509_CERTIFICATES_JSON_KEY_PRIMARY);
            x509Certificates_free(new_x509certs);
            new_x509certs = NULL;
        }
        if (json_deserialize_and_get_struct(&(new_x509certs->secondary), root_object, X509_CERTIFICATES_JSON_KEY_SECONDARY, x509CertificateWithInfo_fromJson, OPTIONAL) != 0)
        {
            LogError("Failed to set '%s' in X509 Certificates", X509_CERTIFICATES_JSON_KEY_SECONDARY);
            x509Certificates_free(new_x509certs);
            new_x509certs = NULL;
        }
    }
    return new_x509certs;
}

static void x509Attestation_free(X509_ATTESTATION* x509_att)
{
    if (x509_att != NULL)
    {
        if (x509_att->type == CERTIFICATE_TYPE_CLIENT)
        {
            if (x509_att->certificates.client_certificates != NULL)
            {
                x509Certificates_free(x509_att->certificates.client_certificates);
            }
        }
        else if (x509_att->type == CERTIFICATE_TYPE_SIGNING)
        {
            if (x509_att->certificates.signing_certificates != NULL)
            {
                x509Certificates_free(x509_att->certificates.signing_certificates);
            }
        }
        else if (x509_att->type == CERTIFICATE_TYPE_CA_REFERENCES)
        {
            if (x509_att->certificates.ca_references != NULL)
            {
                x509CAReferences_free(x509_att->certificates.ca_references);
            }
        }

        free(x509_att);
    }
}

static JSON_Value* x509Attestation_toJson(const X509_ATTESTATION* x509_att)
{
    JSON_Value* root_value = NULL;
    JSON_Object* root_object = NULL;



    //Setup
    if ((root_value = json_value_init_object()) == NULL)
    {
        LogError("json_value_init_object failed");
    }
    else if ((root_object = json_value_get_object(root_value)) == NULL)
    {
        LogError("json_value_get_object failed");
        json_value_free(root_value);
        root_value = NULL;
    }

    //Set data
    else
    {
        if (x509_att->type == CERTIFICATE_TYPE_CLIENT)
        {
            if (json_serialize_and_set_struct(root_object, X509_ATTESTATION_JSON_KEY_CLIENT_CERTS, x509_att->certificates.client_certificates, x509Certificates_toJson, REQUIRED) != 0)
            {
                LogError("Failed to set '%s' in JSON string representation of X509 Attestation", X509_ATTESTATION_JSON_KEY_CLIENT_CERTS);
                json_value_free(root_value);
                root_value = NULL;
            }
        }
        else if (x509_att->type == CERTIFICATE_TYPE_SIGNING)
        {
            if (json_serialize_and_set_struct(root_object, X509_ATTESTATION_JSON_KEY_SIGNING_CERTS, x509_att->certificates.signing_certificates, x509Certificates_toJson, REQUIRED) != 0)
            {
                LogError("Failed to set '%s' in JSON string representation of X509 Attestation", X509_ATTESTATION_JSON_KEY_SIGNING_CERTS);
                json_value_free(root_value);
                root_value = NULL;
            }
        }
        else if (x509_att->type == CERTIFICATE_TYPE_CA_REFERENCES)
        {
            if (json_serialize_and_set_struct(root_object, X509_ATTESTATION_JSON_KEY_CA_REFERENCES, x509_att->certificates.ca_references, x509CAReferences_toJson, REQUIRED) != 0)
            {
                LogError("Failed to set '%s' in JSON string representation of X509 Attestation", X509_ATTESTATION_JSON_KEY_CA_REFERENCES);
                json_value_free(root_value);
                root_value = NULL;
            }
        }
    }

    return root_value;
}

static X509_ATTESTATION* x509Attestation_fromJson(JSON_Object* root_object)
{
    X509_ATTESTATION* new_x509Att = NULL;

    if (root_object == NULL)
    {
        LogError("No X509 Attestation in JSON");
    }
    else if ((new_x509Att = malloc(sizeof(X509_ATTESTATION))) == NULL)
    {
        LogError("Allocation of X509 Attestation failed");
    }
    else
    {
        memset(new_x509Att, 0, sizeof(X509_ATTESTATION));

        if (json_object_has_value(root_object, X509_ATTESTATION_JSON_KEY_CLIENT_CERTS))
        {
            if (json_deserialize_and_get_struct(&(new_x509Att->certificates.client_certificates), root_object, X509_ATTESTATION_JSON_KEY_CLIENT_CERTS, x509Certificates_fromJson, REQUIRED) != 0)
            {
                LogError("Failed to set '%s' in X509 Attestation", X509_ATTESTATION_JSON_KEY_CLIENT_CERTS);
                x509Attestation_free(new_x509Att);
                new_x509Att = NULL;
            }
            else
            {
                new_x509Att->type = CERTIFICATE_TYPE_CLIENT;
            }
        }
        else if (json_object_has_value(root_object, X509_ATTESTATION_JSON_KEY_SIGNING_CERTS))
        {
            if (json_deserialize_and_get_struct(&(new_x509Att->certificates.signing_certificates), root_object, X509_ATTESTATION_JSON_KEY_SIGNING_CERTS, x509Certificates_fromJson, REQUIRED) != 0)
            {
                LogError("Failed to set '%s' in X509 Attestation", X509_ATTESTATION_JSON_KEY_SIGNING_CERTS);
                x509Attestation_free(new_x509Att);
                new_x509Att = NULL;
            }
            else
            {
                new_x509Att->type = CERTIFICATE_TYPE_SIGNING;
            }
        }
        else if (json_object_has_value(root_object, X509_ATTESTATION_JSON_KEY_CA_REFERENCES))
        {
            if (json_deserialize_and_get_struct(&(new_x509Att->certificates.ca_references), root_object, X509_ATTESTATION_JSON_KEY_CA_REFERENCES, x509CAReferences_fromJson, OPTIONAL) != 0)
            {
                LogError("Failed to set '%s' in X509 Attestation", X509_ATTESTATION_JSON_KEY_CA_REFERENCES);
                x509Attestation_free(new_x509Att);
                new_x509Att = NULL;
            }
            else
            {
                new_x509Att->type = CERTIFICATE_TYPE_CA_REFERENCES;
            }
        }
        else
        {
            LogError("No client or signing certificates");
            x509Attestation_free(new_x509Att);
            new_x509Att = NULL;
        }
    }

    return new_x509Att;
}

static X509_ATTESTATION* x509Attestation_create(CERTIFICATE_TYPE cert_type, const char* primary_cert, const char* secondary_cert)
{
    X509_ATTESTATION* new_x509Att = NULL;

    if ((cert_type == CERTIFICATE_TYPE_NONE) || (primary_cert == NULL))
    {
        LogError("Requires valid certificate type and primary certificate to create X509 Attestation");
    }
    else if ((new_x509Att = malloc(sizeof(X509_ATTESTATION))) == NULL)
    {
        LogError("Failed to allocate memory for X509 Attestation");
    }
    else
    {
        memset(new_x509Att, 0, sizeof(X509_ATTESTATION));
        
        new_x509Att->type = cert_type;
        if (cert_type == CERTIFICATE_TYPE_CLIENT)
        {
            if ((new_x509Att->certificates.client_certificates = x509Certificates_create(primary_cert, secondary_cert)) == NULL)
            {
                LogError("Failed to create Client Certificates");
                x509Attestation_free(new_x509Att);
                new_x509Att = NULL;
            }
        }
        else if (cert_type == CERTIFICATE_TYPE_SIGNING)
        {
            if ((new_x509Att->certificates.signing_certificates = x509Certificates_create(primary_cert, secondary_cert)) == NULL)
            {
                LogError("Failed to create Client Certificates");
                x509Attestation_free(new_x509Att);
                new_x509Att = NULL;
            }
        }
        else if (cert_type == CERTIFICATE_TYPE_CA_REFERENCES)
        {
            if ((new_x509Att->certificates.ca_references = x509CAReferences_create(primary_cert, secondary_cert)) == NULL)
            {
                LogError("Failed to create CA References");
                x509Attestation_free(new_x509Att);
                new_x509Att = NULL;
            }
        }
    }

    return new_x509Att;
}

static void tpmAttestation_free(TPM_ATTESTATION* tpm_att)
{
    if (tpm_att != NULL)
    {
        free(tpm_att->endorsement_key);
        free(tpm_att->storage_root_key);
        free(tpm_att);
    }
}

static JSON_Value* tpmAttestation_toJson(const TPM_ATTESTATION* tpm_att)
{
    JSON_Value* root_value = NULL;
    JSON_Object* root_object = NULL;

    //Setup
    if ((root_value = json_value_init_object()) == NULL)
    {
        LogError("json_value_init_object failed");
    }
    else if ((root_object = json_value_get_object(root_value)) == NULL)
    {
        LogError("json_value_get_object failed");
        json_value_free(root_value);
        root_value = NULL;
    }

    //Set data
    else if (json_object_set_string(root_object, TPM_ATTESTATION_JSON_KEY_EK, tpm_att->endorsement_key) != JSONSuccess)
    {
        LogError("Failed to set '%s' in JSON string representation of TPM Attestation", TPM_ATTESTATION_JSON_KEY_EK);
        json_value_free(root_value);
        root_value = NULL;
    }
    else if ((tpm_att->storage_root_key != NULL) && (json_object_set_string(root_object, TPM_ATTESTATION_JSON_KEY_SRK, tpm_att->storage_root_key) != JSONSuccess))
    {
        LogError("Failed to set '%s' in JSON string representation of TPM Attestation", TPM_ATTESTATION_JSON_KEY_SRK);
        json_value_free(root_value);
        root_value = NULL;
    }

    return root_value;
}

static TPM_ATTESTATION* tpmAttestation_fromJson(JSON_Object * root_object)
{
    TPM_ATTESTATION* new_tpmAtt = NULL;

    if (root_object == NULL)
    {
        LogError("No TPM Attestation in JSON");
    }
    else if ((new_tpmAtt = malloc(sizeof(TPM_ATTESTATION))) == NULL)
    {
        LogError("Allocation of TPM Attestation failed");
    }
    else
    {
        memset(new_tpmAtt, 0, sizeof(TPM_ATTESTATION));

        if (copy_json_string_field(&(new_tpmAtt->endorsement_key), root_object, TPM_ATTESTATION_JSON_KEY_EK) != 0)
        {
            LogError("Failed to set '%s' in TPM Attestation", TPM_ATTESTATION_JSON_KEY_EK);
            tpmAttestation_free(new_tpmAtt);
            new_tpmAtt = NULL;
        }
        else if (copy_json_string_field(&(new_tpmAtt->storage_root_key), root_object, TPM_ATTESTATION_JSON_KEY_SRK) != 0)
        {
            LogError("Failed to set '%s' in TPM Attestation", TPM_ATTESTATION_JSON_KEY_SRK);
            tpmAttestation_free(new_tpmAtt);
            new_tpmAtt = NULL;
        }
    }

    return new_tpmAtt;
}

static void attestationMechanism_free(ATTESTATION_MECHANISM* att_mech)
{
    if (att_mech != NULL)
    {
        if (att_mech->type == ATTESTATION_TYPE_TPM)
        {
            if (att_mech->attestation.tpm != NULL)
            {
                tpmAttestation_free(att_mech->attestation.tpm);
            }
        }
        else if (att_mech->type == ATTESTATION_TYPE_X509)
        {
            if (att_mech->attestation.x509 != NULL)
            {
                x509Attestation_free(att_mech->attestation.x509);
            }
        }

        free(att_mech);
    }
}

static JSON_Value* attestationMechanism_toJson(const ATTESTATION_MECHANISM* att_mech)
{
    JSON_Value* root_value = NULL;
    JSON_Object* root_object = NULL;

    const char* at_str = NULL;

    //Setup
    if (att_mech == NULL)
    {
        LogError("enrollment is NULL");
    }
    else if ((root_value = json_value_init_object()) == NULL)
    {
        LogError("json_value_init_object failed");
    }
    else if ((root_object = json_value_get_object(root_value)) == NULL)
    {
        LogError("json_value_get_object failed");
        json_value_free(root_value);
        root_value = NULL;
    }

    //Set data
    else if (((at_str = attestationType_toJson(att_mech->type)) == NULL) || (json_object_set_string(root_object, ATTESTATION_MECHANISM_JSON_KEY_TYPE, at_str) != JSONSuccess))
    {
        LogError("Failed to set '%s' in JSON string representation of Attestation Mechanism", ATTESTATION_MECHANISM_JSON_KEY_TYPE);
        json_value_free(root_value);
        root_value = NULL;
    }
    else
    {
        if (att_mech->type == ATTESTATION_TYPE_TPM)
        {
            if (json_serialize_and_set_struct(root_object, ATTESTATION_MECHANISM_JSON_KEY_TPM, att_mech->attestation.tpm, tpmAttestation_toJson, REQUIRED) != 0)
            {
                LogError("Failed to set '%s' in JSON string representation of Attestation Mechanism", ATTESTATION_MECHANISM_JSON_KEY_TPM);
                json_value_free(root_value);
                root_value = NULL;
            }
        }
        else if (att_mech->type == ATTESTATION_TYPE_X509)
        {
            if (json_serialize_and_set_struct(root_object, ATTESTATION_MECHANISM_JSON_KEY_X509, att_mech->attestation.x509, x509Attestation_toJson, REQUIRED) != 0)
            {
                LogError("Failed to set '%s' in JSON string representation of Attestation Mechanism", ATTESTATION_MECHANISM_JSON_KEY_X509);
                json_value_free(root_value);
                root_value = NULL;
            }
        }
    }


    return root_value;
}

static ATTESTATION_MECHANISM* attestationMechanism_fromJson(JSON_Object* root_object)
{
    ATTESTATION_MECHANISM* new_attMech = NULL;

    if (root_object == NULL)
    {
        LogError("No attestation mechanism in JSON");
    }
    else if ((new_attMech = malloc(sizeof(ATTESTATION_MECHANISM))) == NULL)
    {
        LogError("Allocation of Attestation Mechanism failed");
    }
    else
    {
        memset(new_attMech, 0, sizeof(ATTESTATION_MECHANISM));

        if ((new_attMech->type = attestationType_fromJson(json_object_get_string(root_object, ATTESTATION_MECHANISM_JSON_KEY_TYPE))) == ATTESTATION_TYPE_NONE)
        {
            LogError("Failed to set '%s' in Attestation Mechanism", ATTESTATION_MECHANISM_JSON_KEY_TYPE);
            attestationMechanism_free(new_attMech);
            new_attMech = NULL;
        }
        else if (new_attMech->type == ATTESTATION_TYPE_TPM)
        {
            if (json_deserialize_and_get_struct(&(new_attMech->attestation.tpm), root_object, ATTESTATION_MECHANISM_JSON_KEY_TPM, tpmAttestation_fromJson, REQUIRED) != 0)
            {
                LogError("Failed to set '%s' in Attestation Mechanism", ATTESTATION_MECHANISM_JSON_KEY_TPM);
                attestationMechanism_free(new_attMech);
                new_attMech = NULL;
            }
        }
        else if (new_attMech->type == ATTESTATION_TYPE_X509)
        {
            if (json_deserialize_and_get_struct(&(new_attMech->attestation.x509), root_object, ATTESTATION_MECHANISM_JSON_KEY_X509, x509Attestation_fromJson, REQUIRED) != 0)
            {
                LogError("Failed to set '%s' in Attestation Mechanism", ATTESTATION_MECHANISM_JSON_KEY_X509);
                attestationMechanism_free(new_attMech);
                new_attMech = NULL;
            }
        }
    }

    return new_attMech;
}

static int attestationMechanism_validateForIndividualEnrollment(ATTESTATION_MECHANISM* attmech)
{
    int result;

    if (attmech == NULL)
    {
        result = 0;
    }
    else if (attmech->type == ATTESTATION_TYPE_TPM)
    {
        result = 0;
    }
    else if (attmech->type == ATTESTATION_TYPE_X509)
    {
        if (attmech->attestation.x509->type == CERTIFICATE_TYPE_CLIENT)
        {
            result = 0;
        }
        else
        {
            result = -1;
        }
    }
    else
    {
        result = -1;
    }

    return result;
}

static int attestationMechanism_validateForEnrollmentGroup(ATTESTATION_MECHANISM* attmech)
{
    int result;

    if (attmech == NULL)
    {
        result = 0;
    }
    else if (attmech->type == ATTESTATION_TYPE_TPM)
    {
        result = -1;
    }
    else if (attmech->type == ATTESTATION_TYPE_X509)
    {
        if (attmech->attestation.x509->type == CERTIFICATE_TYPE_SIGNING)
        {
            result = -1;
        }
        else
        {
            result = 0;
        }
    }
    else
    {
        result = -1;
    }

    return result;
}

static void deviceRegistrationState_free(DEVICE_REGISTRATION_STATE* device_reg_state)
{
    if (device_reg_state != NULL)
    {
        free(device_reg_state->registration_id);
        free(device_reg_state->created_date_time_utc);
        free(device_reg_state->device_id);
        free(device_reg_state->updated_date_time_utc);
        free(device_reg_state->error_message);
        free(device_reg_state->etag);
        free(device_reg_state);
    }
}

static DEVICE_REGISTRATION_STATE* deviceRegistrationState_fromJson(JSON_Object* root_object)
{
    DEVICE_REGISTRATION_STATE* new_device_reg_state = NULL;

    if (root_object == NULL)
    {
        LogError("No device registration state in JSON");
    }
    else if ((new_device_reg_state = malloc(sizeof(DEVICE_REGISTRATION_STATE))) == NULL)
    {
        LogError("Allocation of Device Registration State failed");
    }
    else
    {
        memset(new_device_reg_state, 0, sizeof(DEVICE_REGISTRATION_STATE));

        if (copy_json_string_field(&(new_device_reg_state->registration_id), root_object, DEVICE_REGISTRATION_STATE_JSON_KEY_REG_ID) != 0)
        {
            LogError("Failed to set '%s' in Device Registration State", DEVICE_REGISTRATION_STATE_JSON_KEY_REG_ID);
            deviceRegistrationState_free(new_device_reg_state);
            new_device_reg_state = NULL;
        }
        else if (copy_json_string_field(&(new_device_reg_state->created_date_time_utc), root_object, DEVICE_REGISTRATION_STATE_JSON_KEY_CREATED_TIME) != 0)
        {
            LogError("Failed to set '%s' in Device Registration State", DEVICE_REGISTRATION_STATE_JSON_KEY_CREATED_TIME);
            deviceRegistrationState_free(new_device_reg_state);
            new_device_reg_state = NULL;
        }
        else if (copy_json_string_field(&(new_device_reg_state->device_id), root_object, DEVICE_REGISTRATION_STATE_JSON_KEY_DEVICE_ID) != 0)
        {
            LogError("Failed to set '%s' in Device Registration State", DEVICE_REGISTRATION_STATE_JSON_KEY_DEVICE_ID);
            deviceRegistrationState_free(new_device_reg_state);
            new_device_reg_state = NULL;
        }
        else if ((new_device_reg_state->status = registrationStatus_fromJson(json_object_get_string(root_object, DEVICE_REGISTRATION_STATE_JSON_KEY_REG_STATUS))) == REGISTRATION_STATUS_ERROR)
        {
            LogError("Failed to set '%s' in Device Registration State", DEVICE_REGISTRATION_STATE_JSON_KEY_REG_STATUS);
            deviceRegistrationState_free(new_device_reg_state);
            new_device_reg_state = NULL;
        }
        else if (copy_json_string_field(&(new_device_reg_state->updated_date_time_utc), root_object, DEVICE_REGISTRATION_STATE_JSON_KEY_UPDATED_TIME) != 0)
        {
            LogError("Failed to set '%s' in Device Registration State", DEVICE_REGISTRATION_STATE_JSON_KEY_UPDATED_TIME);
            deviceRegistrationState_free(new_device_reg_state);
            new_device_reg_state = NULL;
        }
        else if (copy_json_string_field(&(new_device_reg_state->error_message), root_object, DEVICE_REGISTRATION_STATE_JSON_KEY_ERROR_MSG) != 0)
        {
            LogError("Failed to set '%s' in Device Registration State", DEVICE_REGISTRATION_STATE_JSON_KEY_ERROR_MSG);
            deviceRegistrationState_free(new_device_reg_state);
            new_device_reg_state = NULL;
        }
        else if (copy_json_string_field(&(new_device_reg_state->etag), root_object, DEVICE_REGISTRATION_STATE_JSON_KEY_ETAG) != 0)
        {
            LogError("Failed to set '%s' in Device Registration State", DEVICE_REGISTRATION_STATE_JSON_KEY_ETAG);
            deviceRegistrationState_free(new_device_reg_state);
            new_device_reg_state = NULL;
        }
        else
            new_device_reg_state->error_code = (int)json_object_get_number(root_object, DEVICE_REGISTRATION_STATE_JSON_KEY_ERROR_CODE);
    }

    return new_device_reg_state;
}

static void individualEnrollment_free(INDIVIDUAL_ENROLLMENT* enrollment)
{
    if (enrollment != NULL) {
        free(enrollment->registration_id);
        free(enrollment->device_id);
        free(enrollment->etag);
        free(enrollment->created_date_time_utc);
        free(enrollment->updated_date_time_utc);
        attestationMechanism_free(enrollment->attestation_mechanism);
        initialTwinState_destroy(enrollment->initial_twin);
        deviceRegistrationState_free(enrollment->registration_state);
        free(enrollment);
    }
}

static JSON_Value* individualEnrollment_toJson(const INDIVIDUAL_ENROLLMENT* enrollment)
{
    JSON_Value* root_value = NULL;
    JSON_Object* root_object = NULL;

    const char* ps_str = NULL;

    //Setup
    if (enrollment == NULL)
    {
        LogError("enrollment is NULL");
    }
    else if ((root_value = json_value_init_object()) == NULL)
    {
        LogError("json_value_init_object failed");
    }
    else if ((root_object = json_value_get_object(root_value)) == NULL)
    {
        LogError("json_value_get_object failed");
        json_value_free(root_value);
        root_value = NULL;
    }

    //Set data
    else if (json_object_set_string(root_object, INDIVIDUAL_ENROLLMENT_JSON_KEY_REG_ID, enrollment->registration_id) != JSONSuccess)
    {
        LogError("Failed to set '%s' in JSON string", INDIVIDUAL_ENROLLMENT_JSON_KEY_REG_ID);
        json_value_free(root_value);
        root_value = NULL;
    }
    else if ((enrollment->device_id != NULL) && (json_object_set_string(root_object, INDIVIDUAL_ENROLLMENT_JSON_KEY_DEVICE_ID, enrollment->device_id) != JSONSuccess))
    {
        LogError("Failed to set '%s' in JSON String", INDIVIDUAL_ENROLLMENT_JSON_KEY_DEVICE_ID);
        json_value_free(root_value);
        root_value = NULL;
    }
    else if (json_serialize_and_set_struct(root_object, INDIVIDUAL_ENROLLMENT_JSON_KEY_ATTESTATION, enrollment->attestation_mechanism, attestationMechanism_toJson, REQUIRED) != 0)
    {
        LogError("Failed to set '%s' in JSON String", INDIVIDUAL_ENROLLMENT_JSON_KEY_ATTESTATION);
        json_value_free(root_value);
        root_value = NULL;
    }
    else if (json_serialize_and_set_struct(root_object, INDIVIDUAL_ENROLLMENT_JSON_KEY_INITIAL_TWIN, enrollment->initial_twin, initialTwinState_toJson, OPTIONAL) != 0)
    {
        LogError("Failed to set '%s' in JSON String", INDIVIDUAL_ENROLLMENT_JSON_KEY_INITIAL_TWIN);
        json_value_free(root_value);
        root_value = NULL;
    }
    else if ((enrollment->etag != NULL) && (json_object_set_string(root_object, INDIVIDUAL_ENROLLMENT_JSON_KEY_ETAG, enrollment->etag) != JSONSuccess))
    {
        LogError("Failed to set '%s' in JSON String", INDIVIDUAL_ENROLLMENT_JSON_KEY_ETAG);
        json_value_free(root_value);
        root_value = NULL;
    }
    else if (((ps_str = provisioningStatus_toJson(enrollment->provisioning_status)) == NULL) || (json_object_set_string(root_object, INDIVIDUAL_ENROLLMENT_JSON_KEY_PROV_STATUS, ps_str) != JSONSuccess))
    {
        LogError("Failed to set '%s' in JSON String", INDIVIDUAL_ENROLLMENT_JSON_KEY_PROV_STATUS);
        json_value_free(root_value);
        root_value = NULL;
    }
    //Do not set registration_state, create_date_time_utc or update_date_time_utc as they are READ ONLY

    return root_value;
}

static INDIVIDUAL_ENROLLMENT* individualEnrollment_fromJson(JSON_Object* root_object)
{
    INDIVIDUAL_ENROLLMENT* new_enrollment = NULL;

    if (root_object == NULL)
    {
        LogError("No enrollment in JSON");
    }
    else if ((new_enrollment = malloc(sizeof(INDIVIDUAL_ENROLLMENT))) == NULL)
    {
        LogError("Allocation of Individual Enrollment failed");
    }
    else
    {
        memset(new_enrollment, 0, sizeof(INDIVIDUAL_ENROLLMENT));

        if (copy_json_string_field(&(new_enrollment->registration_id), root_object, INDIVIDUAL_ENROLLMENT_JSON_KEY_REG_ID) != 0)
        {
            LogError("Failed to set '%s' in Individual Enrollment", INDIVIDUAL_ENROLLMENT_JSON_KEY_REG_ID);
            individualEnrollment_free(new_enrollment);
            new_enrollment = NULL;
        }
        else if (copy_json_string_field(&(new_enrollment->device_id), root_object, INDIVIDUAL_ENROLLMENT_JSON_KEY_DEVICE_ID) != 0)
        {
            LogError("Failed to set '%s' in Individual Enrollment", INDIVIDUAL_ENROLLMENT_JSON_KEY_DEVICE_ID);
            individualEnrollment_free(new_enrollment);
            new_enrollment = NULL;
        }
        else if ((json_object_has_value(root_object, INDIVIDUAL_ENROLLMENT_JSON_KEY_REG_STATE)) && (new_enrollment->registration_state = deviceRegistrationState_fromJson(json_object_get_object(root_object, INDIVIDUAL_ENROLLMENT_JSON_KEY_REG_STATE))) == NULL)
        {
            LogError("Failed to set '%s' in Individual Enrollment", INDIVIDUAL_ENROLLMENT_JSON_KEY_REG_STATE);
            individualEnrollment_free(new_enrollment);
            new_enrollment = NULL;
        }
        else if (json_deserialize_and_get_struct(&(new_enrollment->attestation_mechanism), root_object, INDIVIDUAL_ENROLLMENT_JSON_KEY_ATTESTATION, attestationMechanism_fromJson, REQUIRED) != 0)
        {
            LogError("Failed to set '%s' in Individual Enrollment", INDIVIDUAL_ENROLLMENT_JSON_KEY_ATTESTATION);
            individualEnrollment_free(new_enrollment);
            new_enrollment = NULL;
        }
        else if (json_deserialize_and_get_struct(&(new_enrollment->initial_twin), root_object, INDIVIDUAL_ENROLLMENT_JSON_KEY_INITIAL_TWIN, initialTwinState_fromJson, OPTIONAL) != 0)
        {
            LogError("Failed to set '%s' in Individual Enrollment", INDIVIDUAL_ENROLLMENT_JSON_KEY_INITIAL_TWIN);
            individualEnrollment_free(new_enrollment);
            new_enrollment = NULL;
        }
        else if (copy_json_string_field(&(new_enrollment->etag), root_object, INDIVIDUAL_ENROLLMENT_JSON_KEY_ETAG) != 0)
        {
            LogError("Failed to set '%s' in Individual Enrollment", INDIVIDUAL_ENROLLMENT_JSON_KEY_ETAG);
            individualEnrollment_free(new_enrollment);
            new_enrollment = NULL;
        }
        else if ((new_enrollment->provisioning_status = provisioningStatus_fromJson(json_object_get_string(root_object, INDIVIDUAL_ENROLLMENT_JSON_KEY_PROV_STATUS))) == PROVISIONING_STATUS_NONE)
        {
            LogError("Failed to set '%s' in Individual Enrollment", INDIVIDUAL_ENROLLMENT_JSON_KEY_PROV_STATUS);
            individualEnrollment_free(new_enrollment);
            new_enrollment = NULL;
        }
        else if (copy_json_string_field(&(new_enrollment->created_date_time_utc), root_object, INDIVIDUAL_ENROLLMENT_JSON_KEY_CREATED_TIME) != 0)
        {
            LogError("Failed to set '%s' in Individual Enrollment", INDIVIDUAL_ENROLLMENT_JSON_KEY_CREATED_TIME);
            individualEnrollment_free(new_enrollment);
            new_enrollment = NULL;
        }
        else if (copy_json_string_field(&(new_enrollment->updated_date_time_utc), root_object, INDIVIDUAL_ENROLLMENT_JSON_KEY_UPDATED_TIME) != 0)
        {
            LogError("Failed to set '%s' in Individual Enrollment", INDIVIDUAL_ENROLLMENT_JSON_KEY_UPDATED_TIME);
            individualEnrollment_free(new_enrollment);
            new_enrollment = NULL;
        }
    }

    return new_enrollment;
}

static void enrollmentGroup_free(ENROLLMENT_GROUP* enrollment)
{
    if (enrollment != NULL)
    {
        free(enrollment->group_id);
        attestationMechanism_free(enrollment->attestation_mechanism);
        initialTwinState_destroy(enrollment->initial_twin);
        free(enrollment->etag);
        free(enrollment->created_date_time_utc);
        free(enrollment->updated_date_time_utc);
        free(enrollment);
    }
}

static JSON_Value* enrollmentGroup_toJson(const ENROLLMENT_GROUP* enrollment)
{
    JSON_Value* root_value = NULL;
    JSON_Object* root_object = NULL;

    //Setup
    if (enrollment == NULL)
    {
        LogError("enrollment is NULL");
    }
    else if ((root_value = json_value_init_object()) == NULL)
    {
        LogError("json_value_init_object failed");
    }
    else if ((root_object = json_value_get_object(root_value)) == NULL)
    {
        LogError("json_value_get_object failed");
        json_value_free(root_value);
        root_value = NULL;
    }

    //Set data
    else if (json_object_set_string(root_object, ENROLLMENT_GROUP_JSON_KEY_GROUP_ID, enrollment->group_id) != JSONSuccess)
    {
        LogError("Failed to set '%s' in JSON string", ENROLLMENT_GROUP_JSON_KEY_GROUP_ID);
        json_value_free(root_value);
        root_value = NULL;
    }
    else if (json_serialize_and_set_struct(root_object, ENROLLMENT_GROUP_JSON_KEY_ATTESTATION, enrollment->attestation_mechanism, attestationMechanism_toJson, REQUIRED) != 0)
    {
        LogError("Failed to set '%s' in JSON string", ENROLLMENT_GROUP_JSON_KEY_ATTESTATION);
        json_value_free(root_value);
        root_value = NULL;
    }
    else if (json_serialize_and_set_struct(root_object, ENROLLMENT_GROUP_JSON_KEY_INITIAL_TWIN, enrollment->initial_twin, initialTwinState_toJson, OPTIONAL) != 0)
    {
        LogError("Failed to set '%s' in JSON string", ENROLLMENT_GROUP_JSON_KEY_INITIAL_TWIN);
        json_value_free(root_value);
        root_value = NULL;
    }
    else if ((enrollment->etag != NULL) && (json_object_set_string(root_object, ENROLLMENT_GROUP_JSON_KEY_ETAG, enrollment->etag) != JSONSuccess))
    {
        LogError("Failed to set '%s' in JSON string", ENROLLMENT_GROUP_JSON_KEY_ETAG);
        json_value_free(root_value);
        root_value = NULL;
    }
    else if (json_object_set_string(root_object, ENROLLMENT_GROUP_JSON_KEY_PROV_STATUS, provisioningStatus_toJson(enrollment->provisioning_status)) != JSONSuccess)
    {
        LogError("Failed to set '%s' in JSON string", ENROLLMENT_GROUP_JSON_KEY_PROV_STATUS);
        json_value_free(root_value);
        root_value = NULL;
    }

    return root_value;
}

static ENROLLMENT_GROUP* enrollmentGroup_fromJson(JSON_Object* root_object)
{
    ENROLLMENT_GROUP* new_enrollment = NULL;

    if ((new_enrollment = malloc(sizeof(ENROLLMENT_GROUP))) == NULL)
    {
        LogError("Allocation of Enrollment Group failed");
    }
    else
    {
        memset(new_enrollment, 0, sizeof(ENROLLMENT_GROUP));

        if (copy_json_string_field(&(new_enrollment->group_id), root_object, ENROLLMENT_GROUP_JSON_KEY_GROUP_ID) != 0)
        {
            LogError("Failed to set '%s' in Enrollment Group", ENROLLMENT_GROUP_JSON_KEY_GROUP_ID);
            enrollmentGroup_free(new_enrollment);
            new_enrollment = NULL;
        }
        else if (json_deserialize_and_get_struct(&(new_enrollment->attestation_mechanism), root_object, ENROLLMENT_GROUP_JSON_KEY_ATTESTATION, attestationMechanism_fromJson, REQUIRED) != 0)
        {
            LogError("Failed to set '%s' in Enrollment Group", ENROLLMENT_GROUP_JSON_KEY_ATTESTATION);
            enrollmentGroup_free(new_enrollment);
            new_enrollment = NULL;
        }
        else if (json_deserialize_and_get_struct(&(new_enrollment->initial_twin), root_object, ENROLLMENT_GROUP_JSON_KEY_INITIAL_TWIN, initialTwinState_fromJson, OPTIONAL) != 0)
        {
            LogError("Failed to set '%s' in Enrollment Group", ENROLLMENT_GROUP_JSON_KEY_INITIAL_TWIN);
            enrollmentGroup_free(new_enrollment);
            new_enrollment = NULL;
        }
        else if (copy_json_string_field(&(new_enrollment->etag), root_object, ENROLLMENT_GROUP_JSON_KEY_ETAG) != 0)
        {
            LogError("Failed to set '%s' in Enrollment Group", ENROLLMENT_GROUP_JSON_KEY_ETAG);
            enrollmentGroup_free(new_enrollment);
            new_enrollment = NULL;
        }
        else if ((new_enrollment->provisioning_status = provisioningStatus_fromJson(json_object_get_string(root_object, ENROLLMENT_GROUP_JSON_KEY_PROV_STATUS))) == PROVISIONING_STATUS_NONE)
        {
            LogError("Failed to set '%s' in Enrollment Group", ENROLLMENT_GROUP_JSON_KEY_PROV_STATUS);
            enrollmentGroup_free(new_enrollment);
            new_enrollment = NULL;
        }
        else if (copy_json_string_field(&(new_enrollment->created_date_time_utc), root_object, ENROLLMENT_GROUP_JSON_KEY_CREATED_TIME) != 0)
        {
            LogError("Failed to set '%s' in Enrollment Group", ENROLLMENT_GROUP_JSON_KEY_CREATED_TIME);
            enrollmentGroup_free(new_enrollment);
            new_enrollment = NULL;
        }
        else if (copy_json_string_field(&(new_enrollment->updated_date_time_utc), root_object, ENROLLMENT_GROUP_JSON_KEY_UPDATED_TIME) != 0)
        {
            LogError("Failed to set '%s' in Enrollment Group", ENROLLMENT_GROUP_JSON_KEY_UPDATED_TIME);
            enrollmentGroup_free(new_enrollment);
            new_enrollment = NULL;
        }
    }

    return new_enrollment;
}

/* Exposed API Functions*/

ATTESTATION_MECHANISM_HANDLE attestationMechanism_createWithTpm(const char* endorsement_key)
{
    ATTESTATION_MECHANISM* att_mech = NULL;
    TPM_ATTESTATION* tpm_attestation = NULL;

    if (endorsement_key == NULL)
    {
        LogError("endorsement_key is NULL");
    }
    else if ((att_mech = malloc(sizeof(ATTESTATION_MECHANISM))) == NULL)
    {
        LogError("Allocation of Attestation Mechanism failed");
    }
    else if ((tpm_attestation = malloc(sizeof(TPM_ATTESTATION))) == NULL)
    {
        LogError("Allocation of TPM attestation failed");
        free(att_mech);
        att_mech = NULL;
    }
    else
    {
        memset(att_mech, 0, sizeof(ATTESTATION_MECHANISM));
        memset(tpm_attestation, 0, sizeof(TPM_ATTESTATION));

        att_mech->type = ATTESTATION_TYPE_TPM;
        att_mech->attestation.tpm = tpm_attestation;

        if (copy_string(&(tpm_attestation->endorsement_key), endorsement_key) != 0)
        {
            LogError("Setting endorsement key in individual enrollment failed");
            attestationMechanism_free(att_mech);
            att_mech = NULL;
        }
    }

    return (ATTESTATION_MECHANISM_HANDLE)att_mech;
}

ATTESTATION_MECHANISM_HANDLE attestationMechanism_createWithX509ClientCert(const char* primary_cert, const char* secondary_cert)
{
    ATTESTATION_MECHANISM* att_mech = NULL;

    if (primary_cert == NULL)
    {
        LogError("primary_cert is NULL");
    }
    else if ((att_mech = malloc(sizeof(ATTESTATION_MECHANISM))) == NULL)
    {
        LogError("Allocation of Attestation Mechanism failed");
    }
    else
    {
        memset(att_mech, 0, sizeof(ATTESTATION_MECHANISM));

        if ((att_mech->attestation.x509 = x509Attestation_create(CERTIFICATE_TYPE_CLIENT, primary_cert, secondary_cert)) == NULL)
        {
            LogError("Allocation of X509 Attestation failed");
            attestationMechanism_free(att_mech);
            att_mech = NULL;
        }
        else
        {
            att_mech->type = ATTESTATION_TYPE_X509;
        }
    }

    return (ATTESTATION_MECHANISM_HANDLE)att_mech;
}

ATTESTATION_MECHANISM_HANDLE attestationMechanism_createWithX509SigningCert(const char* primary_cert, const char* secondary_cert)
{
    ATTESTATION_MECHANISM* att_mech = NULL;

    if (primary_cert == NULL)
    {
        LogError("primary_cert is NULL");
    }
    else if ((att_mech = malloc(sizeof(ATTESTATION_MECHANISM))) == NULL)
    {
        LogError("Allocation of Attestation Mechanism failed");
    }
    else
    {
        memset(att_mech, 0, sizeof(ATTESTATION_MECHANISM));

        if ((att_mech->attestation.x509 = x509Attestation_create(CERTIFICATE_TYPE_SIGNING, primary_cert, secondary_cert)) == NULL)
        {
            LogError("Allocation of X509 Attestation failed");
            attestationMechanism_free(att_mech);
            att_mech = NULL;
        }
        else
        {
            att_mech->type = ATTESTATION_TYPE_X509;
        }
    }

    return (ATTESTATION_MECHANISM_HANDLE)att_mech;
}

ATTESTATION_MECHANISM_HANDLE attestationMechanism_createWithX509CAReference(const char* primary_ref, const char* secondary_ref)
{
    ATTESTATION_MECHANISM* att_mech = NULL;

    if (primary_ref == NULL)
    {
        LogError("primary_cert is NULL");
    }
    else if ((att_mech = malloc(sizeof(ATTESTATION_MECHANISM))) == NULL)
    {
        LogError("Allocation of Attestation Mechanism failed");
    }
    else
    {
        memset(att_mech, 0, sizeof(ATTESTATION_MECHANISM));

        if ((att_mech->attestation.x509 = x509Attestation_create(CERTIFICATE_TYPE_CA_REFERENCES, primary_ref, secondary_ref)) == NULL)
        {
            LogError("Allocation of X509 Attestation failed");
            attestationMechanism_free(att_mech);
            att_mech = NULL;
        }
        else
        {
            att_mech->type = ATTESTATION_TYPE_X509;
        }
    }

    return (ATTESTATION_MECHANISM_HANDLE)att_mech;
}

void attestationMechanism_destroy(ATTESTATION_MECHANISM_HANDLE att_handle)
{
    ATTESTATION_MECHANISM* att_mech = (ATTESTATION_MECHANISM*)att_handle;
    attestationMechanism_free(att_mech);
}

TPM_ATTESTATION_HANDLE attestationMechanism_getTpmAttestation(ATTESTATION_MECHANISM_HANDLE att_handle)
{
    TPM_ATTESTATION* new_tpm_att = NULL;
    ATTESTATION_MECHANISM* att_mech = (ATTESTATION_MECHANISM*)att_handle;

    if (att_mech == NULL)
    {
        LogError("attestation mechanism is NULL");
    }
    else if (att_mech->type != ATTESTATION_TYPE_TPM)
    {
        LogError("attestation mechanism is not of type TPM");
    }
    else
    {
        new_tpm_att = att_mech->attestation.tpm;
    }

    return (TPM_ATTESTATION_HANDLE)new_tpm_att;
}

X509_ATTESTATION_HANDLE attestationMechanism_getX509Attestation(ATTESTATION_MECHANISM_HANDLE att_handle)
{
    X509_ATTESTATION* new_x509_att = NULL;
    ATTESTATION_MECHANISM* att_mech = (ATTESTATION_MECHANISM*)att_handle;

    if (att_mech == NULL)
    {
        LogError("attestation mechanism is NULL");
    }
    else if (att_mech->type != ATTESTATION_TYPE_X509)
    {
        LogError("attestation mechanism is not of type X509");
    }
    else
    {
        new_x509_att = att_mech->attestation.x509;
    }

    return (X509_ATTESTATION_HANDLE)new_x509_att;
}

INDIVIDUAL_ENROLLMENT_HANDLE individualEnrollment_create(const char* reg_id, ATTESTATION_MECHANISM_HANDLE att_handle)
{
    INDIVIDUAL_ENROLLMENT* new_enrollment = NULL;

    if (reg_id == NULL)
    {
        LogError("reg_id invalid");
    }
    else if (att_handle == NULL)
    {
        LogError("attestation mechanism handle is NULL");
    }
    else if ((new_enrollment = malloc(sizeof(INDIVIDUAL_ENROLLMENT))) == NULL)
    {
        LogError("Allocation of individual enrollment failed");
    }
    else
    {
        memset(new_enrollment, 0, sizeof(INDIVIDUAL_ENROLLMENT));

        if (copy_string(&(new_enrollment->registration_id), reg_id) != 0)
        {
            LogError("Allocation of registration id failed");
            individualEnrollment_free(new_enrollment);
            new_enrollment = NULL;
        }
        else
        {
            new_enrollment->attestation_mechanism = (ATTESTATION_MECHANISM*)att_handle;
            new_enrollment->provisioning_status = PROVISIONING_STATUS_ENABLED;
        }
    }

    return (INDIVIDUAL_ENROLLMENT_HANDLE)new_enrollment;
}

void individualEnrollment_destroy(INDIVIDUAL_ENROLLMENT_HANDLE handle)
{
    INDIVIDUAL_ENROLLMENT* enrollment = (INDIVIDUAL_ENROLLMENT*)handle;
    individualEnrollment_free(enrollment);
}

char* individualEnrollment_serializeToJson(const INDIVIDUAL_ENROLLMENT_HANDLE handle)
{
    char* result = NULL;
    char* serialized_string = NULL;
    JSON_Value* root_value = NULL;
    INDIVIDUAL_ENROLLMENT* enrollment = (INDIVIDUAL_ENROLLMENT*)handle;

    if (enrollment == NULL)
    {
        LogError("Cannot serialize NULL");
    }
    else if ((root_value = individualEnrollment_toJson(enrollment)) == NULL)
    {
        LogError("Creating json object failed");
    }
    else if ((serialized_string = json_serialize_to_string(root_value)) == NULL)
    {
        LogError("Failed to serialize to JSON");
    }
    else if (copy_string(&result, serialized_string) != 0)
    {
        LogError("Failed to copy serialized string");
    }

    if (root_value != NULL)
    {
        json_value_free(root_value); 
        root_value = NULL;
    }
    if (serialized_string != NULL)
    {
        json_free_serialized_string(serialized_string);
        serialized_string = NULL;
    }

    return result;
}

INDIVIDUAL_ENROLLMENT_HANDLE individualEnrollment_deserializeFromJson(const char* json_string)
{
    INDIVIDUAL_ENROLLMENT* new_enrollment = NULL;
    JSON_Value* root_value = NULL;
    JSON_Object* root_object = NULL;

    if (json_string == NULL)
    {
        LogError("Cannot deserialize NULL");
    }
    else if ((root_value = json_parse_string(json_string)) == NULL)
    {
        LogError("Parsing JSON string failed");
    }
    else if ((root_object = json_value_get_object(root_value)) == NULL)
    {
        LogError("Creating JSON object failed");
    }
    else
    {
        if ((new_enrollment = individualEnrollment_fromJson(root_object)) == NULL)
        {
            LogError("Creating new Individual Enrollment failed");
        }
        json_value_free(root_value); //implicitly frees root_object
        root_value = NULL;
    }

    return (INDIVIDUAL_ENROLLMENT_HANDLE)new_enrollment;
}

ENROLLMENT_GROUP_HANDLE enrollmentGroup_create(const char* group_id, ATTESTATION_MECHANISM_HANDLE att_handle)
{
    ENROLLMENT_GROUP* new_enrollment = NULL;
    ATTESTATION_MECHANISM* att_mech = (ATTESTATION_MECHANISM*)att_handle;

    if (group_id == NULL)
    {
        LogError("group id is NULL");
    }
    else if (att_mech == NULL)
    {
        LogError("attestation mechanism is NULL");
    }
    else if (att_mech->type != ATTESTATION_TYPE_X509)
    {
        LogError("Attestation Mechanism of wrong type");
    }
    else if ((new_enrollment = malloc(sizeof(ENROLLMENT_GROUP))) == NULL)
    {
        LogError("Allocation of enrollment group failed");
    }
    else
    {
        memset(new_enrollment, 0, sizeof(ENROLLMENT_GROUP));

        if (copy_string(&(new_enrollment->group_id), group_id) != 0)
        {
            LogError("Allocation of group id failed");
            enrollmentGroup_free(new_enrollment);
            new_enrollment = NULL;
        }
        else
        {
            new_enrollment->attestation_mechanism = att_mech;
            new_enrollment->provisioning_status = PROVISIONING_STATUS_ENABLED;
        }
    }

    return (ENROLLMENT_GROUP_HANDLE)new_enrollment;
}

void enrollmentGroup_destroy(ENROLLMENT_GROUP_HANDLE handle)
{
    ENROLLMENT_GROUP* enrollment = (ENROLLMENT_GROUP*)handle;
    enrollmentGroup_free(enrollment);
}

char* enrollmentGroup_serializeToJson(ENROLLMENT_GROUP_HANDLE handle)
{
    char* result = NULL;
    char* serialized_string = NULL;
    JSON_Value* root_value = NULL;
    ENROLLMENT_GROUP* enrollment = (ENROLLMENT_GROUP*)handle;

    if (enrollment == NULL)
    {
        LogError("Cannot serialize NULL");
    }
    else if ((root_value = enrollmentGroup_toJson(enrollment)) == NULL)
    {
        LogError("Creating json object failed");
    }
    else if ((serialized_string = json_serialize_to_string(root_value)) == NULL)
    {
        LogError("Serializing to JSON failed");
    }
    else if (copy_string(&result, serialized_string) != 0)
    {
        LogError("Failed to copy serialized string");
    }

    if (root_value != NULL)
    {
        json_value_free(root_value);
        root_value = NULL;
    }

    if (serialized_string != NULL)
    {
        json_free_serialized_string(serialized_string);
        serialized_string = NULL;
    }

    return result;
}

ENROLLMENT_GROUP_HANDLE enrollmentGroup_deserializeFromJson(const char* json_string)
{
    ENROLLMENT_GROUP* new_enrollment = NULL;
    JSON_Value* root_value = NULL;
    JSON_Object* root_object = NULL;

    if (json_string == NULL)
    {
        LogError("Cannot deserialize NULL");
    }
    else if ((root_value = json_parse_string(json_string)) == NULL)
    {
        LogError("Parsong JSON string failed");
    }
    else if ((root_object = json_value_get_object(root_value)) == NULL)
    {
        LogError("Creating JSON object failed");
    }
    else
    {
        if ((new_enrollment = enrollmentGroup_fromJson(root_object)) == NULL)
        {
            LogError("Creating new Enrollment Group failed");
        }
        json_value_free(root_value); //implicitly frees root_object
        root_value = NULL;
    }

    return (ENROLLMENT_GROUP_HANDLE)new_enrollment;
}

/* Accessor Functions - Attestation Mechanism */
ATTESTATION_TYPE attestationMechanism_getType(ATTESTATION_MECHANISM_HANDLE att_handle)
{
    ATTESTATION_TYPE result = ATTESTATION_TYPE_NONE;

    if (att_handle == NULL)
    {
        LogError("attestation mechanism is NULL");
    }
    else
    {
        result = ((ATTESTATION_MECHANISM*)att_handle)->type;
    }

    return result;
}

/*Accessor Functions - Individual Enrollment*/
ATTESTATION_MECHANISM_HANDLE individualEnrollment_getAttestationMechanism(INDIVIDUAL_ENROLLMENT_HANDLE handle)
{
    ATTESTATION_MECHANISM* result = NULL;
    INDIVIDUAL_ENROLLMENT* enrollment = (INDIVIDUAL_ENROLLMENT*)handle;

    if (enrollment == NULL)
    {
        LogError("enrollment is NULL");
    }
    else
    {
        result = enrollment->attestation_mechanism;
    }

    return (ATTESTATION_MECHANISM_HANDLE)result;
}

int individualEnrollment_setAttestationMechanism(INDIVIDUAL_ENROLLMENT_HANDLE ie_handle, ATTESTATION_MECHANISM_HANDLE am_handle)
{
    int result = 0;
    INDIVIDUAL_ENROLLMENT* enrollment = (INDIVIDUAL_ENROLLMENT*)ie_handle;
    ATTESTATION_MECHANISM* attmech = (ATTESTATION_MECHANISM*)am_handle;

    if (enrollment == NULL)
    {
        LogError("enrollment handle is NULL");
        result = __FAILURE__;
    }
    else if (attestationMechanism_validateForIndividualEnrollment(attmech) != 0)
    {
        LogError("Invalid attestation mechanism for Individual Enrollment");
        result = __FAILURE__;
    }
    else
    {
        attestationMechanism_free(enrollment->attestation_mechanism);
        enrollment->attestation_mechanism = attmech;
    }

    return result;
}

INITIAL_TWIN_HANDLE individualEnrollment_getInitialTwinState(INDIVIDUAL_ENROLLMENT_HANDLE handle)
{
    INITIAL_TWIN_HANDLE result = NULL;
    INDIVIDUAL_ENROLLMENT* enrollment = (INDIVIDUAL_ENROLLMENT*)handle;

    if (enrollment == NULL)
    {
        LogError("enrollment is NULL");
    }
    else
    {
        result = enrollment->initial_twin;
    }

    return result;
}

int individualEnrollment_setInitialTwinState(INDIVIDUAL_ENROLLMENT_HANDLE ie_handle, INITIAL_TWIN_HANDLE ts_handle)
{
    int result = 0;
    INDIVIDUAL_ENROLLMENT* enrollment = (INDIVIDUAL_ENROLLMENT*)ie_handle;

    if (enrollment == NULL)
    {
        LogError("enrollment handle is NULL");
        result = __FAILURE__;
    }
    else
    {
        initialTwinState_destroy(enrollment->initial_twin);
        enrollment->initial_twin = ts_handle;
    }

    return result;
}

DEVICE_REGISTRATION_STATE_HANDLE individualEnrollment_getDeviceRegistrationState(INDIVIDUAL_ENROLLMENT_HANDLE handle)
{
    DEVICE_REGISTRATION_STATE* result = NULL;
    INDIVIDUAL_ENROLLMENT* enrollment = (INDIVIDUAL_ENROLLMENT*)handle;

    if (enrollment == NULL)
    {
        LogError("enrollment is NULL");
    }
    else
    {
        result = enrollment->registration_state;
    }

    return (DEVICE_REGISTRATION_STATE_HANDLE)result;
}

const char* individualEnrollment_getRegistrationId(INDIVIDUAL_ENROLLMENT_HANDLE handle)
{
    char* result = NULL;
    INDIVIDUAL_ENROLLMENT* enrollment = (INDIVIDUAL_ENROLLMENT*)handle;

    if (enrollment == NULL)
    {
        LogError("enrollment is NULL");
    }
    else
    {
        result = enrollment->registration_id;
    }

    return result;
}

const char* individualEnrollment_getDeviceId(INDIVIDUAL_ENROLLMENT_HANDLE handle)
{
    char* result = NULL;
    INDIVIDUAL_ENROLLMENT* enrollment = (INDIVIDUAL_ENROLLMENT*)handle;

    if (enrollment == NULL)
    {
        LogError("enrollment is NULL");
    }
    else
    {
        result = enrollment->device_id;
    }

    return result;
}

int individualEnrollment_setDeviceId(INDIVIDUAL_ENROLLMENT_HANDLE handle, const char* device_id)
{
    int result = 0;
    INDIVIDUAL_ENROLLMENT* enrollment = (INDIVIDUAL_ENROLLMENT*)handle;

    if (enrollment == NULL)
    {
        LogError("handle is NULL");
        result = __FAILURE__;
    }
    else if (device_id == NULL)
    {
        LogError("Invalid device id");
        result = __FAILURE__;
    }
    else if (copy_string(&(enrollment->device_id), device_id) != 0)
    {
        LogError("Failed to set device id");
        result = __FAILURE__;
    }

    return result;
}

const char* individualEnrollment_getEtag(INDIVIDUAL_ENROLLMENT_HANDLE handle)
{
    char* result = NULL;
    INDIVIDUAL_ENROLLMENT* enrollment = (INDIVIDUAL_ENROLLMENT*)handle;

    if (enrollment == NULL)
    {
        LogError("enrollment is NULL");
    }
    else
    {
        result = enrollment->etag;
    }

    return result;
}

int individualEnrollment_setEtag(INDIVIDUAL_ENROLLMENT_HANDLE handle, const char* etag)
{
    int result = 0;
    INDIVIDUAL_ENROLLMENT* enrollment = (INDIVIDUAL_ENROLLMENT*)handle;

    if (enrollment == NULL)
    {
        LogError("Invalid handle");
        result = __FAILURE__;
    }
    else if (etag == NULL)
    {
        LogError("Invalid etag");
        result = __FAILURE__;
    }
    else if (copy_string(&(enrollment->etag), etag) != 0)
    {
        LogError("Failed to set etag");
        result = __FAILURE__;
    }

    return result;
}

PROVISIONING_STATUS individualEnrollment_getProvisioningStatus(INDIVIDUAL_ENROLLMENT_HANDLE handle)
{
    PROVISIONING_STATUS result = PROVISIONING_STATUS_NONE;
    INDIVIDUAL_ENROLLMENT* enrollment = (INDIVIDUAL_ENROLLMENT*)handle;

    if (enrollment == NULL)
    {
        LogError("enrollment is NULL");
    }
    else
    {
        result = enrollment->provisioning_status;
    }

    return result;
}

int individualEnrollment_setProvisioningStatus(INDIVIDUAL_ENROLLMENT_HANDLE handle, PROVISIONING_STATUS prov_status)
{
    int result = 0;
    INDIVIDUAL_ENROLLMENT* enrollment = (INDIVIDUAL_ENROLLMENT*)handle;

    if (enrollment == NULL)
    {
        LogError("Invalid handle");
        result = __FAILURE__;
    }
    else if (prov_status == PROVISIONING_STATUS_NONE)
    {
        LogError("Invalid provisioning status");
        result = __FAILURE__;
    }
    else
    {
        enrollment->provisioning_status = prov_status;
    }

    return result;
}

const char* individualEnrollment_getCreatedDateTime(INDIVIDUAL_ENROLLMENT_HANDLE handle)
{
    char* result = NULL;
    INDIVIDUAL_ENROLLMENT* enrollment = (INDIVIDUAL_ENROLLMENT*)handle;

    if (enrollment == NULL)
    {
        LogError("enrollment is NULL");
    }
    else
    {
        result = enrollment->created_date_time_utc;
    }

    return result;
}

const char* individualEnrollment_getUpdatedDateTime(INDIVIDUAL_ENROLLMENT_HANDLE handle)
{
    char* result = NULL;
    INDIVIDUAL_ENROLLMENT* enrollment = (INDIVIDUAL_ENROLLMENT*)handle;

    if (enrollment == NULL)
    {
        LogError("enrollment is NULL");
    }
    else
    {
        result = enrollment->updated_date_time_utc;
    }

    return result;
}

/*Accessor Functions - Enrollment Group*/

ATTESTATION_MECHANISM_HANDLE enrollmentGroup_getAttestationMechanism(ENROLLMENT_GROUP_HANDLE handle)
{
    ATTESTATION_MECHANISM* result = NULL;
    ENROLLMENT_GROUP* enrollment = (ENROLLMENT_GROUP*)handle;

    if (enrollment == NULL)
    {
        LogError("enrollment is NULL");
    }
    else
    {
        result = enrollment->attestation_mechanism;
    }

    return (ATTESTATION_MECHANISM_HANDLE)result;
}

int enrollmentGroup_setAttestationMechanism(ENROLLMENT_GROUP_HANDLE eg_handle, ATTESTATION_MECHANISM_HANDLE am_handle)
{
    int result = 0;
    ENROLLMENT_GROUP* enrollment = (ENROLLMENT_GROUP*)eg_handle;
    ATTESTATION_MECHANISM* attmech = (ATTESTATION_MECHANISM*)am_handle;

    if (enrollment == NULL)
    {
        LogError("enrollment handle is NULL");
        result = __FAILURE__;
    }
    else if (attestationMechanism_validateForEnrollmentGroup(attmech) != 0)
    {
        LogError("Attestation Mechanism is invalid for Enrollment Group");
        result = __FAILURE__;
    }
    else
    {
        attestationMechanism_free(enrollment->attestation_mechanism);
        enrollment->attestation_mechanism = attmech;
    }

    return result;
}

INITIAL_TWIN_HANDLE enrollmentGroup_getInitialTwinState(ENROLLMENT_GROUP_HANDLE handle)
{
    INITIAL_TWIN_HANDLE result = NULL;
    ENROLLMENT_GROUP* enrollment = (ENROLLMENT_GROUP*)handle;

    if (enrollment == NULL)
    {
        LogError("enrollment is NULL");
    }
    else
    {
        result = enrollment->initial_twin;
    }

    return result;
}

int enrollmentGroup_setInitialTwinState(ENROLLMENT_GROUP_HANDLE eg_handle, INITIAL_TWIN_HANDLE ts_handle)
{
    int result = 0;
    ENROLLMENT_GROUP* enrollment = (ENROLLMENT_GROUP*)eg_handle;

    if (enrollment == NULL)
    {
        LogError("enrollment handle is NULL");
        result = __FAILURE__;
    }
    else
    {
        initialTwinState_destroy(enrollment->initial_twin);
        enrollment->initial_twin = ts_handle;
    }

    return result;
}

const char* enrollmentGroup_getGroupId(ENROLLMENT_GROUP_HANDLE handle)
{
    char* result = NULL;
    ENROLLMENT_GROUP* enrollment = (ENROLLMENT_GROUP*)handle;

    if (enrollment == NULL)
    {
        LogError("enrollment is NULL");
    }
    else
    {
        result = enrollment->group_id;
    }

    return result;
}

const char* enrollmentGroup_getEtag(ENROLLMENT_GROUP_HANDLE handle)
{
    char* result = NULL;
    ENROLLMENT_GROUP* enrollment = (ENROLLMENT_GROUP*)handle;

    if (enrollment == NULL)
    {
        LogError("enrollment is NULL");
    }
    else
    {
        result = enrollment->etag;
    }

    return result;
}

int enrollmentGroup_setEtag(ENROLLMENT_GROUP_HANDLE handle, const char* etag)
{
    int result = 0;
    ENROLLMENT_GROUP* enrollment = (ENROLLMENT_GROUP*)handle;

    if (enrollment == NULL)
    {
        LogError("Invalid handle");
        result = __FAILURE__;
    }
    else if (etag == NULL)
    {
        LogError("Invalid etag");
        result = __FAILURE__;
    }
    else if (copy_string(&(enrollment->etag), etag) != 0)
    {
        LogError("Failed to set etag");
        result = __FAILURE__;
    }

    return result;
}

PROVISIONING_STATUS enrollmentGroup_getProvisioningStatus(ENROLLMENT_GROUP_HANDLE handle)
{
    PROVISIONING_STATUS result = PROVISIONING_STATUS_NONE;
    ENROLLMENT_GROUP* enrollment = (ENROLLMENT_GROUP*)handle;

    if (enrollment == NULL)
    {
        LogError("enrollment is NULL");
    }
    else
    {
        result = enrollment->provisioning_status;
    }

    return result;
}

int enrollmentGroup_setProvisioningStatus(ENROLLMENT_GROUP_HANDLE handle, PROVISIONING_STATUS prov_status)
{
    int result = 0;
    ENROLLMENT_GROUP* enrollment = (ENROLLMENT_GROUP*)handle;

    if (enrollment == NULL)
    {
        LogError("Invalid handle");
        result = __FAILURE__;
    }
    else if (prov_status == PROVISIONING_STATUS_NONE)
    {
        LogError("Invalid provisioning status");
        result = __FAILURE__;
    }
    else
    {
        enrollment->provisioning_status = prov_status;
    }

    return result;
}

/* Acessor Functions - Device Registration State */

const char* deviceRegistrationState_getRegistrationId(DEVICE_REGISTRATION_STATE_HANDLE handle)
{
    char* result = NULL;
    DEVICE_REGISTRATION_STATE* drs = (DEVICE_REGISTRATION_STATE*)handle;

    if (drs == NULL)
    {
        LogError("device registration state is NULL");
    }
    else
    {
        result = drs->registration_id;
    }

    return result;
}

const char* deviceRegistrationState_getCreatedDateTime(DEVICE_REGISTRATION_STATE_HANDLE handle)
{
    char* result = NULL;
    DEVICE_REGISTRATION_STATE* drs = (DEVICE_REGISTRATION_STATE*)handle;

    if (drs == NULL)
    {
        LogError("device registration state is NULL");
    }
    else
    {
        result = drs->created_date_time_utc;
    }

    return result;
}

const char* deviceRegistrationState_getDeviceId(DEVICE_REGISTRATION_STATE_HANDLE handle)
{
    char* result = NULL;
    DEVICE_REGISTRATION_STATE* drs = (DEVICE_REGISTRATION_STATE*)handle;

    if (drs == NULL)
    {
        LogError("device registration state is NULL");
    }
    else
    {
        result = drs->device_id;
    }

    return result;
}

REGISTRATION_STATUS deviceRegistrationState_getRegistrationStatus(DEVICE_REGISTRATION_STATE_HANDLE handle)
{
    REGISTRATION_STATUS result = REGISTRATION_STATUS_ERROR;
    DEVICE_REGISTRATION_STATE* drs = (DEVICE_REGISTRATION_STATE*)handle;

    if (drs == NULL)
    {
        LogError("device registration state is NULL");
    }
    else
    {
        result = drs->status;
    }

    return result;
}

const char* deviceRegistrationState_getUpdatedDateTime(DEVICE_REGISTRATION_STATE_HANDLE handle)
{
    char* result = NULL;
    DEVICE_REGISTRATION_STATE* drs = (DEVICE_REGISTRATION_STATE*)handle;

    if (drs == NULL)
    {
        LogError("device registration state is NULL");
    }
    else
    {
        result = drs->updated_date_time_utc;
    }

    return result;
}

int deviceRegistrationState_getErrorCode(DEVICE_REGISTRATION_STATE_HANDLE handle)
{
    int result = 0;
    DEVICE_REGISTRATION_STATE* drs = (DEVICE_REGISTRATION_STATE*)handle;

    if (drs == NULL)
    {
        LogError("device registration state is NULL");
    }
    else
    {
        result = drs->error_code;
    }

    return result;
}

const char* deviceRegistrationState_getErrorMessage(DEVICE_REGISTRATION_STATE_HANDLE handle)
{
    char* result = NULL;
    DEVICE_REGISTRATION_STATE* drs = (DEVICE_REGISTRATION_STATE*)handle;

    if (drs == NULL)
    {
        LogError("device registration state is NULL");
    }
    else
    {
        result = drs->error_message;
    }

    return result;
}

const char* deviceRegistrationState_getEtag(DEVICE_REGISTRATION_STATE_HANDLE handle)
{
    char* result = NULL;
    DEVICE_REGISTRATION_STATE* drs = (DEVICE_REGISTRATION_STATE*)handle;

    if (drs == NULL)
    {
        LogError("device registration state is NULL");
    }
    else
    {
        result = drs->etag;
    }

    return result;
}

/*Accessor Functions - TPM Attestation */

const char* tpmAttestation_getEndorsementKey(TPM_ATTESTATION_HANDLE handle)
{
    char* result = NULL;
    TPM_ATTESTATION* tpm_att = (TPM_ATTESTATION*)handle;

    if (tpm_att != NULL)
    {
        result = tpm_att->endorsement_key;
    }

    return result;
}

/*Acessor Functions - X509 Attestation*/

X509_CERTIFICATE_HANDLE x509Attestation_getPrimaryCertificate(X509_ATTESTATION_HANDLE handle)
{
    X509_CERTIFICATE_WITH_INFO* result = NULL;
    X509_ATTESTATION* x509_att = (X509_ATTESTATION*)handle;

    if (x509_att == NULL)
    {
        LogError("x509 attestation is NULL");
    }
    else
    {
        if (x509_att->type == CERTIFICATE_TYPE_CLIENT)
        {
            if (x509_att->certificates.client_certificates != NULL)
            {
                result = x509_att->certificates.client_certificates->primary;
            }
            else
            {
                LogError("No certificate");
            }
        }
        else if (x509_att->type == CERTIFICATE_TYPE_SIGNING)
        {
            if (x509_att->certificates.signing_certificates != NULL)
            {
                result = x509_att->certificates.signing_certificates->primary;
            }
            else
            {
            LogError("No certificate");
            }
        }
        else
        {
            LogError("invalid certificate type");
        }
    }

    return (X509_CERTIFICATE_HANDLE)result;
}

X509_CERTIFICATE_HANDLE x509Attestation_getSecondaryCertificate(X509_ATTESTATION_HANDLE handle)
{
    X509_CERTIFICATE_WITH_INFO* result = NULL;
    X509_ATTESTATION* x509_att = (X509_ATTESTATION*)handle;

    if (x509_att == NULL)
    {
        LogError("x509 attestation is NULL");
    }
    else
    {
        if (x509_att->type == CERTIFICATE_TYPE_CLIENT)
        {
            if (x509_att->certificates.client_certificates != NULL)
            {
                result = x509_att->certificates.client_certificates->secondary;
            }
            else
            {
                LogError("No certificate");
            }
        }
        else if (x509_att->type == CERTIFICATE_TYPE_SIGNING)
        {
            if (x509_att->certificates.signing_certificates != NULL)
            {
                result = x509_att->certificates.signing_certificates->secondary;
            }
            else
            {
                LogError("No certificate");
            }
        }
        else
        {
            LogError("invalid certificate type");
        }
    }

    return (X509_CERTIFICATE_HANDLE)result;
}

const char* x509Certificate_getSubjectName(X509_CERTIFICATE_HANDLE handle)
{
    char* result = NULL;
    X509_CERTIFICATE_WITH_INFO* x509_certwinfo = (X509_CERTIFICATE_WITH_INFO*)handle;

    if ((x509_certwinfo == NULL) || (x509_certwinfo->info == NULL))
    {
        LogError("Certificate Info is NULL");
    }
    else
    {
        result = x509_certwinfo->info->subject_name;
    }

    return result;
}

const char* x509Certificate_getSha1Thumbprint(X509_CERTIFICATE_HANDLE handle)
{
    char* result = NULL;
    X509_CERTIFICATE_WITH_INFO* x509_certwinfo = (X509_CERTIFICATE_WITH_INFO*)handle;

    if ((x509_certwinfo == NULL) || (x509_certwinfo->info == NULL))
    {
        LogError("Certificate Info is NULL");
    }
    else
    {
        result = x509_certwinfo->info->sha1_thumbprint;
    }

    return result;
}

const char* x509Certificate_getSha256Thumbprint(X509_CERTIFICATE_HANDLE handle)
{
    char* result = NULL;
    X509_CERTIFICATE_WITH_INFO* x509_certwinfo = (X509_CERTIFICATE_WITH_INFO*)handle;

    if ((x509_certwinfo == NULL) || (x509_certwinfo->info == NULL))
    {
        LogError("Certificate Info is NULL");
    }
    else
    {
        result = x509_certwinfo->info->sha256_thumbprint;
    }

    return result;
}

const char* x509Certificate_getIssuerName(X509_CERTIFICATE_HANDLE handle)
{
    char* result = NULL;
    X509_CERTIFICATE_WITH_INFO* x509_certwinfo = (X509_CERTIFICATE_WITH_INFO*)handle;

    if ((x509_certwinfo == NULL) || (x509_certwinfo->info == NULL))
    {
        LogError("Certificate Info is NULL");
    }
    else
    {
        result = x509_certwinfo->info->issuer_name;
    }

    return result;
}

const char* x509Certificate_getNotBeforeUtc(X509_CERTIFICATE_HANDLE handle)
{
    char* result = NULL;
    X509_CERTIFICATE_WITH_INFO* x509_certwinfo = (X509_CERTIFICATE_WITH_INFO*)handle;

    if ((x509_certwinfo == NULL) || (x509_certwinfo->info == NULL))
    {
        LogError("Certificate Info is NULL");
    }
    else
    {
        result = x509_certwinfo->info->not_before_utc;
    }

    return result;
}

const char* x509Certificate_getNotAfterUtc(X509_CERTIFICATE_HANDLE handle)
{
    char* result = NULL;
    X509_CERTIFICATE_WITH_INFO* x509_certwinfo = (X509_CERTIFICATE_WITH_INFO*)handle;

    if ((x509_certwinfo == NULL) || (x509_certwinfo->info == NULL))
    {
        LogError("Certificate Info is NULL");
    }
    else
    {
        result = x509_certwinfo->info->not_after_utc;
    }

    return result;
}

const char* x509Certificate_getSerialNumber(X509_CERTIFICATE_HANDLE handle)
{
    char* result = NULL;
    X509_CERTIFICATE_WITH_INFO* x509_certwinfo = (X509_CERTIFICATE_WITH_INFO*)handle;

    if ((x509_certwinfo == NULL) || (x509_certwinfo->info == NULL))
    {
        LogError("Certificate Info is NULL");
    }
    else
    {
        result = x509_certwinfo->info->serial_number;
    }

    return result;
}

int x509Certificate_getVersion(X509_CERTIFICATE_HANDLE handle)
{
    int result = 0;
    X509_CERTIFICATE_WITH_INFO* x509_certwinfo = (X509_CERTIFICATE_WITH_INFO*)handle;

    if ((x509_certwinfo == NULL) || (x509_certwinfo->info == NULL))
    {
        LogError("Certificate Info is NULL");
    }
    else
    {
        result = x509_certwinfo->info->version;
    }

    return result;
}
