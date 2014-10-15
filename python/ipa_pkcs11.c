/*
 * Copyright (C) 2014  Red Hat
 * Author: Petr Spacek <pspacek@redhat.com>
 * Author: Martin Basti <mbasti@redhat.com>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
 *
 * This code is based on PKCS#11 code snippets from NLnetLabs:
 * http://www.nlnetlabs.nl/publications/hsm/examples/pkcs11/
 * Original license follows:
 */

#include <Python.h>
#include "structmember.h"

#include <pkcs11.h>
#include <openssl/asn1.h>
#include <openssl/x509.h>
#include <openssl/evp.h>
#include <openssl/rsa.h>
#include <openssl/bn.h>
#include <openssl/bio.h>

#include "library.h"

// compat TODO
#define CKM_AES_KEY_WRAP           (0x1090)
#define CKM_AES_KEY_WRAP_PAD       (0x1091)

// TODO
#define CKA_COPYABLE           (0x0017)

CK_BBOOL true = CK_TRUE;
CK_BBOOL false = CK_FALSE;

/**
 * IPA_PKCS11 type
 */
typedef struct {
	PyObject_HEAD
	CK_SLOT_ID slot;
	CK_FUNCTION_LIST_PTR p11;
	CK_SESSION_HANDLE session;
} IPA_PKCS11;


typedef enum {
    sec_en_cka_copyable=0,
    sec_en_cka_decrypt=1,
    sec_en_cka_derive=2,
    sec_en_cka_encrypt=3,
    sec_en_cka_extractable=4,
    sec_en_cka_modifiable=5,
    sec_en_cka_private=6,
    sec_en_cka_sensitive=7,
    sec_en_cka_sign=8,
    sec_en_cka_unwrap=9,
    sec_en_cka_verify=10,
    sec_en_cka_wrap=11,
    sec_en_cka_wrap_with_trusted=12
} secrect_key_enum;

typedef enum {
    pub_en_cka_copyable=0,
    pub_en_cka_derive=1,
    pub_en_cka_encrypt=2,
    pub_en_cka_modifiable=3,
    pub_en_cka_private=4,
    pub_en_cka_trusted=5,
    pub_en_cka_verify=6,
    pub_en_cka_verify_recover=7,
    pub_en_cka_wrap=8
} public_key_enum;

typedef enum {
    priv_en_cka_always_authenticate=0,
    priv_en_cka_copyable=1,
    priv_en_cka_decrypt=2,
    priv_en_cka_derive=3,
    priv_en_cka_extractable=4,
    priv_en_cka_modifiable=5,
    priv_en_cka_private=6,
    priv_en_cka_sensitive=7,
    priv_en_cka_sign=8,
    priv_en_cka_sign_recover=9,
    priv_en_cka_unwrap=10,
    priv_en_cka_wrap_with_trusted=11
} private_key_enum;

typedef struct {
	PyObject* py_obj;
	CK_BBOOL* bool;
} PyObj2Bool_mapping_t;

/**
 * IPA_PKCS11 Exceptions
 */
static PyObject *IPA_PKCS11Exception; //parent class for all exceptions

static PyObject *IPA_PKCS11Error;  //general error
static PyObject *IPA_PKCS11NotFound;  //key not found
static PyObject *IPA_PKCS11DuplicationError; //key already exists


/***********************************************************************
 * Support functions
 */

CK_BBOOL* pyobj_to_bool(PyObject* pyobj){
	if(PyObject_IsTrue(pyobj))
		return &true;
	return &false;

}

void convert_py2bool(PyObj2Bool_mapping_t* mapping, int length){
	int i;
    for(i=0; i < length; ++i){
    	PyObject* py_obj = mapping[i].py_obj;
    	if(py_obj != NULL){
    		Py_INCREF(py_obj);
    		mapping[i].bool = pyobj_to_bool(py_obj);
    		Py_DECREF(py_obj);
    	}
    }
}

/**
 * Convert a unicode string to the utf8 encoded char array
 * :param unicode: input python unicode object
 * :param l length: of returned string
 * Returns NULL if an error occurs, else pointer to string
 */
unsigned char* unicode_to_char_array(PyObject *unicode, Py_ssize_t *l){
	PyObject* utf8_str = PyUnicode_AsUTF8String(unicode);
	if (utf8_str == NULL){
		PyErr_SetString(IPA_PKCS11Error, "Unable to encode UTF-8");
		return NULL;
	}
	Py_XINCREF(utf8_str);
	unsigned char* bytes = (unsigned char*) PyString_AS_STRING(utf8_str);
	if (bytes == NULL){
		PyErr_SetString(IPA_PKCS11Error, "Unable to get bytes from string");
		*l = 0;
	} else {
		*l = PyString_Size(utf8_str);
	}
	Py_XDECREF(utf8_str);
	return bytes;
}

/**
 * Convert utf-8 encoded char array to unicode object
 */
PyObject* char_array_to_unicode(const char* array, unsigned long l){
	return PyUnicode_DecodeUTF8(array, l, "strict");
}

/**
 * Tests result value of pkc11 operations
 * :return: 1 if everything is ok, 0 if an error occurs and set the error message
 */
int check_return_value(CK_RV rv, const char *message) {
	char* errmsg = NULL;
	if (rv != CKR_OK) {
		if (asprintf(&errmsg, "Error at %s: 0x%x\n", message, (unsigned int) rv)
				== -1) {
			PyErr_SetString(IPA_PKCS11Error,
					"DOUBLE ERROR: Creating the error message caused an error");
			return 0; //
		}
		if (errmsg != NULL) {
			PyErr_SetString(IPA_PKCS11Error, errmsg);
			free(errmsg);
		}
		return 0;
	}
	return 1;
}

/*
 * Find keys with specified attributes ('id' or 'label' and 'class' are required)
 *
 *
 * Function return only one key, if more keys meet the search parameters,
 * exception will be raised
 *
 * :param id: key ID, (if value is NULL, will not be used to find key)
 * :param id_len: key ID length
 * :param label key: label (if value is NULL, will not be used to find key)
 * :param label_len: key label length
 * :param class key: class
 * :param cka_wrap:  (if value is NULL, will not be used to find key)
 * :param cka_unwrap: (if value is NULL, will not be used to find key)
 * :param objects: found objects, NULL if no objects fit criteria
 * :param objects_count: number of objects in objects array
 * :return: 1 if success, otherwise return 0 and set the exception
 */
int
_find_key(IPA_PKCS11* self, CK_BYTE_PTR id, CK_ULONG id_len,
		CK_BYTE_PTR label, CK_ULONG label_len,
		CK_OBJECT_CLASS class, CK_BBOOL *cka_wrap,
		CK_BBOOL *cka_unwrap, CK_OBJECT_HANDLE **objects,
		unsigned int *objects_count)
{
	/* specify max number of possible attributes, increase this number whenever
	 * new attribute is added and don't forget to increase attr_count with each
	 * set attribute
	 */
	unsigned int max_possible_attributes = 5;
	CK_ATTRIBUTE template[max_possible_attributes];
	CK_OBJECT_HANDLE result_object;
    CK_ULONG objectCount;
	CK_OBJECT_HANDLE *result_objects = NULL;
	CK_OBJECT_HANDLE *tmp_objects_ptr = NULL;
	unsigned int attr_count = 0;
	unsigned int count = 0;
	unsigned int allocated = 0;
    CK_RV rv;

    if (label!=NULL){
    	template[attr_count].type = CKA_LABEL;
    	template[attr_count].pValue = (void *) label;
    	template[attr_count].ulValueLen = label_len;
    	++attr_count;
    }
    if (id!=NULL){
    	template[attr_count].type = CKA_ID;
    	template[attr_count].pValue = (void *) id;
    	template[attr_count].ulValueLen = id_len;
    	++attr_count;
    }
    if (cka_wrap!=NULL){
    	template[attr_count].type = CKA_WRAP;
    	template[attr_count].pValue = (void *) cka_wrap;
    	template[attr_count].ulValueLen = sizeof(CK_BBOOL);
    	++attr_count;
    }
    if (cka_unwrap!=NULL){
    	template[attr_count].type = CKA_UNWRAP;
    	template[attr_count].pValue = (void *) cka_unwrap;
    	template[attr_count].ulValueLen = sizeof(CK_BBOOL);
    	++attr_count;
    }

    /* Set CLASS */
	template[attr_count].type = CKA_CLASS;
	template[attr_count].pValue = (void *) &class;
	template[attr_count].ulValueLen = sizeof(CK_OBJECT_CLASS);
	++attr_count;


    rv = self->p11->C_FindObjectsInit(self->session, template, attr_count);
    if(!check_return_value(rv, "Find key init"))
    	return 0;


    rv = self->p11->C_FindObjects(self->session, &result_object, 1, &objectCount);
    if(!check_return_value(rv, "Find key"))
    	return 0;

    while (objectCount > 0){
    	if(allocated <= count){
    		allocated += 32;
        	tmp_objects_ptr = (CK_OBJECT_HANDLE*) realloc(result_objects,
        			allocated * sizeof(CK_OBJECT_HANDLE));
        	if (tmp_objects_ptr == NULL){
        		*objects_count = 0;
        		PyErr_SetString(IPA_PKCS11Error, "_find_key realloc failed");
        		if (result_objects != NULL)
        			free(result_objects);
        		return 0;
        	} else {
        		result_objects = tmp_objects_ptr;
        	}
    	}
    	result_objects[count] = result_object;
    	count++;
		rv = self->p11->C_FindObjects(self->session, &result_object, 1, &objectCount);
		if(!check_return_value(rv, "Check for duplicated key")){
    		if (result_objects != NULL)
    			free(result_objects);
			return 0;
		}
    }

    rv = self->p11->C_FindObjectsFinal(self->session);
    if(!check_return_value(rv, "Find objects final")){
		if (result_objects != NULL)
			free(result_objects);
		return 0;
    }

    *objects = result_objects;
    *objects_count = count;
    return 1;
}

/*
 * Find key with specified attributes ('id' or 'label' and 'class' are required)
 *
 * id or label and class must be specified
 *
 * Function return only one key, if more keys meet the search parameters,
 * exception will be raised
 *
 * :param id: key ID, (if value is NULL, will not be used to find key)
 * :param id_len: key ID length
 * :param label key: label (if value is NULL, will not be used to find key)
 * :param label_len: key label length
 * :param class key: class
 * :param cka_wrap:  (if value is NULL, will not be used to find key)
 * :param cka_unwrap: (if value is NULL, will not be used to find key)
 * :param object:
 * :return: 1 if object was found, otherwise return 0 and set the exception
 *
 * :raise IPA_PKCS11NotFound: if no result is returned
 * :raise IPA_PKCS11DuplicationError: if more then 1 key meet the parameters
 */
int
_get_key(IPA_PKCS11* self, CK_BYTE_PTR id, CK_ULONG id_len,
        CK_BYTE_PTR label, CK_ULONG label_len,
        CK_OBJECT_CLASS class, CK_BBOOL *cka_wrap,
        CK_BBOOL *cka_unwrap, CK_OBJECT_HANDLE *object)
{
    /* specify max number of possible attributes, increase this number whenever
     * new attribute is added and don't forget to increase attr_count with each
     * set attribute
     */
    CK_OBJECT_HANDLE* result_objects = NULL;
    unsigned int objects_count = 0;
    unsigned int not_found_err = 0;
    unsigned int duplication_err = 0;
    int r;

    if((label==NULL) && (id==NULL)){
        PyErr_SetString(IPA_PKCS11Error, "Key 'id' or 'label' required.");
        return 0;
    }

    r = _find_key(self, id, id_len, label, label_len, class, cka_wrap,
            cka_unwrap, &result_objects, &objects_count);

    if (!r) return 0;

    not_found_err = (objects_count == 0) ? 1 : 0;
    duplication_err = (objects_count > 1) ? 1 : 0;

    if(!(not_found_err || duplication_err))
        *object = result_objects[0];

    if(result_objects != NULL)
        free(result_objects);

    if (not_found_err) {
        PyErr_SetString(IPA_PKCS11NotFound, "Key not found");
        return 0;
    }

    if (duplication_err) {
        PyErr_SetString(IPA_PKCS11DuplicationError, "_get_key: more than 1 key found");
        return 0;
    }

    return 1;
}

/*
 * Test if object with specified label, id and class exists
 *
 * :param id: key ID, (if value is NULL, will not be used to find key)
 * :param id_len: key ID length
 * :param label key: label (if value is NULL, will not be used to find key)
 * :param label_len: key label length
 * :param class key: class

 * :return: 1 if object was found, 0 if object doesnt exists, -1 if error
 * and set the exception
 *
 */
int
_id_label_exists(IPA_PKCS11* self, CK_BYTE_PTR id, CK_ULONG id_len,
		CK_BYTE_PTR label, CK_ULONG label_len,
		CK_OBJECT_CLASS class) {

    CK_RV rv;
    CK_ULONG object_count = 0;
    CK_OBJECT_HANDLE result_object = 0;
    CK_ATTRIBUTE template[] = {
         {CKA_ID, id, id_len},
         {CKA_LABEL, label, label_len},
         {CKA_CLASS, &class, sizeof(CK_OBJECT_CLASS)},
    };

    rv = self->p11->C_FindObjectsInit(self->session, template, 3);
    if(!check_return_value(rv, "id, label exists init"))
    	return -1;

    rv = self->p11->C_FindObjects(self->session, &result_object, 1, &object_count);
    if(!check_return_value(rv, "id, label exists"))
    	return -1;

    rv = self->p11->C_FindObjectsFinal(self->session);
    if(!check_return_value(rv, "id, label exists final"))
    	return -1;

    if (object_count == 0){
    	return 0;
    }
    return 1; /* Object found*/
}

/***********************************************************************
 * IPA_PKCS11 object
 */

static void IPA_PKCS11_dealloc(IPA_PKCS11* self) {
	self->ob_type->tp_free((PyObject*) self);
}

static PyObject *
IPA_PKCS11_new(PyTypeObject *type, PyObject *args, PyObject *kwds) {
	IPA_PKCS11 *self;

	self = (IPA_PKCS11 *) type->tp_alloc(type, 0);
	if (self != NULL) {

		self->slot = 0;
		self->session = 0;
		self->p11 = NULL;
	}

	return (PyObject *) self;
}

static int IPA_PKCS11_init(IPA_PKCS11 *self, PyObject *args, PyObject *kwds) {

	static char *kwlist[] = { NULL };
	if (!PyArg_ParseTupleAndKeywords(args, kwds, "|", kwlist))
		return -1;

	return 0;
}

static PyMemberDef IPA_PKCS11_members[] = { { NULL } /* Sentinel */
};

/**
 * Initialization PKC11 library
 */
static PyObject *
IPA_PKCS11_initialize(IPA_PKCS11* self, PyObject *args) {
	const char* user_pin = NULL;
	const char* library_path = NULL;
	CK_RV rv;
	void *module_handle = NULL;

	/* Parse method args*/
	if (!PyArg_ParseTuple(args, "iss", &self->slot, &user_pin, &library_path))
		return NULL;

	CK_C_GetFunctionList pGetFunctionList = loadLibrary(library_path,
			&module_handle);
	if (!pGetFunctionList) {
		PyErr_SetString(IPA_PKCS11Error, "Could not load the library.");
		return NULL;
	}

	/*
	 * Load the function list
	 */
	(*pGetFunctionList)(&self->p11);

	/*
	 * Initialize
	 */
	rv = self->p11->C_Initialize(NULL);
	if (!check_return_value(rv, "initialize"))
		return NULL;

	/*
	 *Start session
	 */
	rv = self->p11->C_OpenSession(self->slot,
			CKF_SERIAL_SESSION | CKF_RW_SESSION, NULL, NULL, &self->session);
	if (!check_return_value(rv, "open session"))
		return NULL;

	/*
	 * Login
	 */
	rv = self->p11->C_Login(self->session, CKU_USER, (CK_BYTE*) user_pin,
			strlen((char *) user_pin));
	if (!check_return_value(rv, "log in"))
		return NULL;

	return Py_None;
}

/*
 * Finalize operations with pkcs11 library
 */
static PyObject *
IPA_PKCS11_finalize(IPA_PKCS11* self) {
	CK_RV rv;

	if (self->p11 == NULL)
		return Py_None;

	/*
	 * Logout
	 */
	rv = self->p11->C_Logout(self->session);
	if (rv != CKR_USER_NOT_LOGGED_IN) {
		if (!check_return_value(rv, "log out"))
			return NULL;
	}

	/*
	 * End session
	 */
	rv = self->p11->C_CloseSession(self->session);
	if (!check_return_value(rv, "close session"))
		return NULL;

	/*
	 * Finalize
	 */
	self->p11->C_Finalize(NULL);

	self->p11 = NULL;
	self->session = 0;
	self->slot = 0;

	return Py_None;
}


/********************************************************************
 * Methods working with keys
 */

/**
 * Generate master key
 *
 *:return: master key handle
 */
static PyObject *
IPA_PKCS11_generate_master_key(IPA_PKCS11* self, PyObject *args, PyObject *kwds)
{

    PyObj2Bool_mapping_t attrs[] = {
        {NULL, &true}, //sec_en_cka_copyable
        {NULL, &false}, //sec_en_cka_decrypt
        {NULL, &false}, //sec_en_cka_derive
        {NULL, &false}, //sec_en_cka_encrypt
        {NULL, &true}, //sec_en_cka_extractable
        {NULL, &true}, //sec_en_cka_modifiable
        {NULL, &true}, //sec_en_cka_private
        {NULL, &true}, //sec_en_cka_sensitive
        {NULL, &false}, //sec_en_cka_sign
        {NULL, &true}, //sec_en_cka_unwrap
        {NULL, &false}, //sec_en_cka_verify
        {NULL, &true}, //sec_en_cka_wrap
        {NULL, &false} //sec_en_cka_wrap_with_trusted
    };

    CK_ULONG key_length = 16;
    CK_RV rv;
    CK_OBJECT_HANDLE master_key;
    CK_BYTE *id = NULL;
    int id_length = 0;

    PyObject *label_unicode = NULL;
    Py_ssize_t label_length = 0;
    int r;
	static char *kwlist[] = {"subject", "id", "key_length", "cka_copyable",
			"cka_decrypt", "cka_derive", "cka_encrypt", "cka_extractable",
			"cka_modifiable", "cka_private", "cka_sensitive", "cka_sign",
			"cka_unwrap", "cka_verify", "cka_wrap", "cka_wrap_with_trusted", NULL };
	//TODO check long overflow
	if (!PyArg_ParseTupleAndKeywords(args, kwds, "Us#|kOOOOOOOOOOOOO", kwlist,
			&label_unicode, &id, &id_length, &key_length,
			&attrs[sec_en_cka_copyable].py_obj,
			&attrs[sec_en_cka_decrypt].py_obj,
			&attrs[sec_en_cka_derive].py_obj,
			&attrs[sec_en_cka_encrypt].py_obj,
			&attrs[sec_en_cka_extractable].py_obj,
			&attrs[sec_en_cka_modifiable].py_obj,
			&attrs[sec_en_cka_private].py_obj,
			&attrs[sec_en_cka_sensitive].py_obj,
			&attrs[sec_en_cka_sign].py_obj,
			&attrs[sec_en_cka_unwrap].py_obj,
			&attrs[sec_en_cka_verify].py_obj,
			&attrs[sec_en_cka_wrap].py_obj,
			&attrs[sec_en_cka_wrap_with_trusted].py_obj)){
		return NULL;
	}

	Py_XINCREF(label_unicode);
	CK_BYTE *label = (unsigned char*) unicode_to_char_array(label_unicode, &label_length);
	Py_XDECREF(label_unicode);
    CK_MECHANISM mechanism = { //TODO param?
         CKM_AES_KEY_GEN, NULL_PTR, 0
    };

    if ((key_length != 16) && (key_length != 24) && (key_length != 32)){
        PyErr_SetString(IPA_PKCS11Error, "generate_master_key: key length allowed values are: 16, 24 and 32");
        return NULL;
    }

    //TODO free label if check failed
    //TODO is label freed inside???? dont we use freed value later
    r = _id_label_exists(self, id, id_length, label, label_length, CKO_SECRET_KEY);
    if (r == 1){
    	PyErr_SetString(IPA_PKCS11DuplicationError,
    			"Master key with same ID and LABEL already exists");
    	return NULL;
    } else if (r == -1){
    	return NULL;
    }

    /* Process keyword boolean arguments */
    convert_py2bool(attrs,
                    sizeof(attrs)/sizeof(PyObj2Bool_mapping_t));


    CK_ATTRIBUTE symKeyTemplate[] = {
         {CKA_ID, id, id_length},
         {CKA_LABEL, label, label_length},
         {CKA_TOKEN, &true, sizeof(CK_BBOOL)},
         {CKA_VALUE_LEN, &key_length, sizeof(key_length)},
         //{CKA_COPYABLE, attrs[sec_en_cka_copyable].bool, sizeof(CK_BBOOL)}, //TODO Softhsm doesn't support it
         {CKA_DECRYPT, attrs[sec_en_cka_decrypt].bool, sizeof(CK_BBOOL)},
         {CKA_DERIVE, attrs[sec_en_cka_derive].bool, sizeof(CK_BBOOL)},
         {CKA_ENCRYPT, attrs[sec_en_cka_encrypt].bool, sizeof(CK_BBOOL)},
         {CKA_EXTRACTABLE, attrs[sec_en_cka_extractable].bool, sizeof(CK_BBOOL)},
         {CKA_MODIFIABLE, attrs[sec_en_cka_modifiable].bool, sizeof(CK_BBOOL)},
         {CKA_PRIVATE, attrs[sec_en_cka_private].bool, sizeof(CK_BBOOL)},
         {CKA_SENSITIVE, attrs[sec_en_cka_sensitive].bool, sizeof(CK_BBOOL)},
         {CKA_SIGN, attrs[sec_en_cka_sign].bool, sizeof(CK_BBOOL)},
         {CKA_UNWRAP, attrs[sec_en_cka_unwrap].bool, sizeof(CK_BBOOL)},
         {CKA_VERIFY, attrs[sec_en_cka_verify].bool, sizeof(CK_BBOOL)},
         {CKA_WRAP, attrs[sec_en_cka_wrap].bool, sizeof(CK_BBOOL)},
         {CKA_WRAP_WITH_TRUSTED, attrs[sec_en_cka_wrap_with_trusted].bool, sizeof(CK_BBOOL)}
    };

    rv = self->p11->C_GenerateKey(self->session,
                           &mechanism,
                           symKeyTemplate,
			    sizeof(symKeyTemplate)/sizeof(CK_ATTRIBUTE),
                           &master_key);
    if(!check_return_value(rv, "generate master key"))
    	return NULL;

	return Py_BuildValue("k",master_key);;
}


/**
 * Generate replica keys
 *
 * :returns: tuple (public_key_handle, private_key_handle)
 */
static PyObject *
IPA_PKCS11_generate_replica_key_pair(IPA_PKCS11* self, PyObject *args, PyObject *kwds)
{
    CK_RV rv;
    int r;
    CK_ULONG modulus_bits = 2048;
    CK_BYTE *id = NULL;
    int id_length = 0;
    PyObject* label_unicode = NULL;
    Py_ssize_t label_length = 0;

    PyObj2Bool_mapping_t attrs_pub[] = {
        {NULL, &true}, //pub_en_cka_copyable
        {NULL, &false}, //pub_en_cka_derive
        {NULL, &false}, //pub_en_cka_encrypt
        {NULL, &true}, //pub_en_cka_modifiable
        {NULL, &true}, //pub_en_cka_private
        {NULL, &false}, //pub_en_cka_trusted
        {NULL, &false}, //pub_en_cka_verify
        {NULL, &false}, //pub_en_cka_verify_recover
        {NULL, &true}, //pub_en_cka_wrap
    };

    PyObj2Bool_mapping_t attrs_priv[] = {
        {NULL, &false}, //priv_en_cka_always_authenticate
        {NULL, &true}, //priv_en_cka_copyable
        {NULL, &false}, //priv_en_cka_decrypt
        {NULL, &false}, //priv_en_cka_derive
        {NULL, &false}, //priv_en_cka_extractable
        {NULL, &true}, //priv_en_cka_modifiable
        {NULL, &true}, //priv_en_cka_private
        {NULL, &true}, //priv_en_cka_sensitive
        {NULL, &false}, //priv_en_cka_sign
        {NULL, &false}, //priv_en_cka_sign_recover
        {NULL, &true}, //priv_en_cka_unwrap
        {NULL, &false} //priv_en_cka_wrap_with_trusted
    };

    static char *kwlist[] = {"label", "id", "modulus_bits",
            /* public key kw */
            "pub_cka_copyable", "pub_cka_derive", "pub_cka_encrypt",
            "pub_cka_modifiable", "pub_cka_private",
            "pub_cka_trusted", "pub_cka_verify", "pub_cka_verify_recover",
            "pub_cka_wrap",
            /* private key kw*/
            "priv_cka_always_authenticate",
            "priv_cka_copyable", "priv_cka_decrypt", "priv_cka_derive",
            "priv_cka_extractable", "priv_cka_modifiable",
            "priv_cka_private",
            "priv_cka_sensitive", "priv_cka_sign", "priv_cka_sign_recover",
            "priv_cka_unwrap", "priv_cka_wrap_with_trusted", NULL };

     if (!PyArg_ParseTupleAndKeywords(args, kwds, "Us#|kOOOOOOOOOOOOOOOOOOOOO",
            kwlist, &label_unicode, &id, &id_length, &modulus_bits,
            /* public key kw */
            &attrs_pub[pub_en_cka_copyable].py_obj,
            &attrs_pub[pub_en_cka_derive].py_obj,
            &attrs_pub[pub_en_cka_encrypt].py_obj,
            &attrs_pub[pub_en_cka_modifiable].py_obj,
            &attrs_pub[pub_en_cka_private].py_obj,
            &attrs_pub[pub_en_cka_trusted].py_obj,
            &attrs_pub[pub_en_cka_verify].py_obj,
            &attrs_pub[pub_en_cka_verify_recover].py_obj,
            &attrs_pub[pub_en_cka_wrap].py_obj,
            /* private key kw*/
            &attrs_priv[priv_en_cka_always_authenticate].py_obj,
            &attrs_priv[priv_en_cka_copyable].py_obj,
            &attrs_priv[priv_en_cka_decrypt].py_obj,
            &attrs_priv[priv_en_cka_derive].py_obj,
            &attrs_priv[priv_en_cka_extractable].py_obj,
            &attrs_priv[priv_en_cka_modifiable].py_obj,
            &attrs_priv[priv_en_cka_private].py_obj,
            &attrs_priv[priv_en_cka_sensitive].py_obj,
            &attrs_priv[priv_en_cka_sign].py_obj,
            &attrs_priv[priv_en_cka_sign_recover].py_obj,
            &attrs_priv[priv_en_cka_unwrap].py_obj,
            &attrs_priv[priv_en_cka_wrap_with_trusted].py_obj)){
		return NULL;
	}

	Py_XINCREF(label_unicode);
	CK_BYTE *label = unicode_to_char_array(label_unicode, &label_length);
	Py_XDECREF(label_unicode);

    CK_OBJECT_HANDLE public_key, private_key;
    CK_MECHANISM mechanism = {
         CKM_RSA_PKCS_KEY_PAIR_GEN, NULL_PTR, 0
    };

    //TODO free variables

    r = _id_label_exists(self, id, id_length, label, label_length, CKO_PRIVATE_KEY);
    if (r == 1){
    	PyErr_SetString(IPA_PKCS11DuplicationError,
    			"Private key with same ID and LABEL already exists");
    	return NULL;
    } else if (r == -1){
    	return NULL;
    }

    r = _id_label_exists(self, id, id_length, label, label_length, CKO_PUBLIC_KEY);
    if (r == 1){
    	PyErr_SetString(IPA_PKCS11DuplicationError,
    			"Public key with same ID and LABEL already exists");
    	return NULL;
    } else if (r == -1){
    	return NULL;
    }

    /* Process keyword boolean arguments */
    convert_py2bool(attrs_pub,
                sizeof(attrs_pub)/sizeof(PyObj2Bool_mapping_t));
    convert_py2bool(attrs_priv,
                sizeof(attrs_priv)/sizeof(PyObj2Bool_mapping_t));

    CK_BYTE public_exponent[] = { 1, 0, 1 }; /* 65537 (RFC 6376 section 3.3.1)*/
    CK_ATTRIBUTE publicKeyTemplate[] = {
         {CKA_ID, id, id_length},
         {CKA_LABEL, label, label_length},
         {CKA_TOKEN, &true, sizeof(true)},
         {CKA_MODULUS_BITS, &modulus_bits, sizeof(modulus_bits)},
         {CKA_PUBLIC_EXPONENT, public_exponent, 3},
         //{CKA_COPYABLE, attrs_pub[pub_en_cka_copyable].bool, sizeof(CK_BBOOL)}, //TODO Softhsm doesn't support it
         {CKA_DERIVE, attrs_pub[pub_en_cka_derive].bool, sizeof(CK_BBOOL)},
         {CKA_ENCRYPT, attrs_pub[pub_en_cka_encrypt].bool, sizeof(CK_BBOOL)},
         {CKA_MODIFIABLE, attrs_pub[pub_en_cka_modifiable].bool, sizeof(CK_BBOOL)},
         {CKA_PRIVATE, attrs_pub[pub_en_cka_private].bool, sizeof(CK_BBOOL)},
         {CKA_TRUSTED, attrs_pub[pub_en_cka_trusted].bool, sizeof(CK_BBOOL)},
         {CKA_VERIFY, attrs_pub[pub_en_cka_verify].bool, sizeof(CK_BBOOL)},
         {CKA_VERIFY_RECOVER, attrs_pub[pub_en_cka_verify_recover].bool, sizeof(CK_BBOOL)},
         {CKA_WRAP, attrs_pub[pub_en_cka_wrap].bool, sizeof(CK_BBOOL)},
    };
    CK_ATTRIBUTE privateKeyTemplate[] = {
         {CKA_ID, id, id_length},
         {CKA_LABEL, label, label_length},
         {CKA_TOKEN, &true, sizeof(true)},
         {CKA_ALWAYS_AUTHENTICATE, attrs_priv[priv_en_cka_always_authenticate].bool, sizeof(CK_BBOOL)},
         //{CKA_COPYABLE, attrs_priv[priv_en_cka_copyable].bool, sizeof(CK_BBOOL)}, //TODO Softhsm doesn't support it
         {CKA_DECRYPT, attrs_priv[priv_en_cka_decrypt].bool, sizeof(CK_BBOOL)},
         {CKA_DERIVE, attrs_priv[priv_en_cka_derive].bool, sizeof(CK_BBOOL)},
         {CKA_EXTRACTABLE, attrs_priv[priv_en_cka_extractable].bool, sizeof(CK_BBOOL)},
         {CKA_MODIFIABLE, attrs_priv[priv_en_cka_modifiable].bool, sizeof(CK_BBOOL)},
         {CKA_PRIVATE, attrs_priv[priv_en_cka_private].bool, sizeof(CK_BBOOL)},
         {CKA_SENSITIVE, attrs_priv[priv_en_cka_sensitive].bool, sizeof(CK_BBOOL)},
         {CKA_SIGN, attrs_priv[priv_en_cka_sign].bool, sizeof(CK_BBOOL)},
         {CKA_SIGN_RECOVER, attrs_priv[priv_en_cka_sign].bool, sizeof(CK_BBOOL)},
         {CKA_UNWRAP, attrs_priv[priv_en_cka_unwrap].bool, sizeof(CK_BBOOL)},
         {CKA_WRAP_WITH_TRUSTED, attrs_priv[priv_en_cka_wrap_with_trusted].bool, sizeof(CK_BBOOL)}
    };

    rv = self->p11->C_GenerateKeyPair(self->session,
                           &mechanism,
                           publicKeyTemplate,
			    sizeof(publicKeyTemplate)/sizeof(CK_ATTRIBUTE),
                           privateKeyTemplate,
			    sizeof(privateKeyTemplate)/sizeof(CK_ATTRIBUTE),
                           &public_key,
                           &private_key);
    if(!check_return_value(rv, "generate key pair"))
    	return NULL;

	return Py_BuildValue("(kk)", public_key, private_key);
}

/**
 * Get key
 *
 * Default class: public_key
 *
 */
static PyObject *
IPA_PKCS11_get_key_handle(IPA_PKCS11* self, PyObject *args, PyObject *kwds)
{
    CK_OBJECT_CLASS class = CKO_PUBLIC_KEY;
    CK_BYTE *id = NULL;
    CK_BBOOL *cka_wrap = NULL;
    CK_BBOOL *cka_unwrap = NULL;
    int id_length = 0;
    PyObject *label_unicode = NULL;
    PyObject *cka_wrap_obj = NULL;
    PyObject *cka_unwrap_obj = NULL;
    Py_ssize_t label_length = 0;
    static char *kwlist[] = {"class", "label", "id", "cka_wrap", "cka_unwrap", NULL };

    if (!PyArg_ParseTupleAndKeywords(args, kwds, "i|Uz#OO", kwlist,
            &class, &label_unicode, &id, &id_length,
            &cka_wrap_obj, &cka_unwrap_obj)){
        return NULL;
    }

	CK_BYTE *label = NULL;
	if (label_unicode != NULL){
		Py_INCREF(label_unicode);
		label = (unsigned char*) unicode_to_char_array(label_unicode, &label_length); //TODO verify signed/unsigned
		Py_DECREF(label_unicode);
	}

	if(cka_wrap_obj!=NULL){
		Py_INCREF(cka_wrap_obj);
		if (PyObject_IsTrue(cka_wrap_obj)){
			cka_wrap = &true;
		} else {
			cka_wrap = &false;
		}
		Py_DECREF(cka_wrap_obj);
	}

	if(cka_unwrap_obj!=NULL){
		Py_INCREF(cka_unwrap_obj);
		if (PyObject_IsTrue(cka_unwrap_obj)){
			cka_unwrap = &true;
		} else {
			cka_unwrap = &false;
		}
		Py_DECREF(cka_unwrap_obj);
	}

	CK_OBJECT_HANDLE object = 0;
	if(! _get_key(self, id, id_length, label, label_length, class, cka_wrap,
			cka_unwrap, &object))
		return NULL;

	return Py_BuildValue("k",object);
}


/**
 * Find key
 *
 * Default class: public_key
 *
 */
static PyObject *
IPA_PKCS11_find_keys(IPA_PKCS11* self, PyObject *args, PyObject *kwds)
{
    CK_OBJECT_CLASS class = CKO_PUBLIC_KEY;
    CK_BYTE *id = NULL; //TODO free
    CK_BBOOL *ckawrap = NULL;
    CK_BBOOL *ckaunwrap = NULL;
    int id_length = 0;
    PyObject *label_unicode = NULL;
    PyObject *cka_wrap_bool = NULL;
    PyObject *cka_unwrap_bool = NULL;
    Py_ssize_t label_length = 0;
    CK_OBJECT_HANDLE *objects = NULL;
    unsigned int objects_len = 0;
    PyObject *result_list = NULL;
	CK_BYTE *label = NULL; //TODO free

	static char *kwlist[] = {"class", "label", "id", "cka_wrap", "cka_unwrap", NULL };
	//TODO check long overflow
	if (!PyArg_ParseTupleAndKeywords(args, kwds, "i|Uz#OO", kwlist,
			 &class, &label_unicode, &id, &id_length,
			 &cka_wrap_bool, &cka_unwrap_bool)){
		return NULL;
	}

	if (label_unicode != NULL){
		Py_INCREF(label_unicode);
		label = (unsigned char*) unicode_to_char_array(label_unicode, &label_length); //TODO verify signed/unsigned
		Py_DECREF(label_unicode);
	}

	if(cka_wrap_bool!=NULL){
		Py_INCREF(cka_wrap_bool);
		if (PyObject_IsTrue(cka_wrap_bool)){
			ckawrap = &true;
		} else {
			ckawrap = &false;
		}
		Py_DECREF(cka_wrap_bool);
	}

	if(cka_unwrap_bool!=NULL){
		Py_INCREF(cka_unwrap_bool);
		if (PyObject_IsTrue(cka_unwrap_bool)){
			ckaunwrap = &true;
		} else {
			ckaunwrap = &false;
		}
		Py_DECREF(cka_unwrap_bool);
	}

	if(! _find_key(self, id, id_length, label, label_length, class, ckawrap,
			ckaunwrap, &objects, &objects_len))
		return NULL;

	result_list = PyList_New(objects_len);
	if(result_list == NULL){
    	PyErr_SetString(IPA_PKCS11Error,
    			"Unable to create list with results");
    	if(objects != NULL){
    		free(objects);
    	}
    	return NULL;
	}
	Py_INCREF(result_list);
	for(int i=0; i<objects_len; ++i){
		if(PyList_SetItem(result_list, i, Py_BuildValue("k",objects[i])) == -1){
	    	PyErr_SetString(IPA_PKCS11Error,
	    			"Unable to add to value to result list");
	    	if(objects != NULL){
	    		free(objects);
	    	}
	    	return NULL;
		}
	}

	return result_list;
}


/**
 * delete key
 */
static PyObject *
IPA_PKCS11_delete_key(IPA_PKCS11* self, PyObject *args, PyObject *kwds)
{
	CK_RV rv;
    CK_OBJECT_HANDLE key_handle = 0;
	static char *kwlist[] = {"key_handle", NULL };
	//TODO check long overflow
	if (!PyArg_ParseTupleAndKeywords(args, kwds, "k|", kwlist,
			 &key_handle)){
		return NULL;
	}
	rv = self->p11->C_DestroyObject(self->session, key_handle);
	if(!check_return_value(rv, "object deletion")){
		return NULL;
	}

	return Py_None;
}

/**
 * export secret key
 */
//TODO remove, we don't want to export secret key
static PyObject *
IPA_PKCS11_export_secret_key(IPA_PKCS11* self, PyObject *args, PyObject *kwds)
{
	CK_RV rv;
	CK_UTF8CHAR_PTR value = NULL;
    CK_OBJECT_HANDLE key_handle = 0;
    PyObject *ret = NULL;
	static char *kwlist[] = {"key_handle", NULL };
	//TODO check long overflow
	if (!PyArg_ParseTupleAndKeywords(args, kwds, "k|", kwlist,
			 &key_handle)){
		return NULL;
	}

	//TODO which attributes should be returned ????
    CK_ATTRIBUTE obj_template[] = {
         {CKA_VALUE, NULL_PTR, 0}
    };

    rv = self->p11->C_GetAttributeValue(self->session, key_handle, obj_template, 1);
    if (!check_return_value(rv, "get attribute value - prepare")){
    	return NULL;
    }

    /* Set proper size for attributes*/
    value = (CK_UTF8CHAR_PTR) malloc(obj_template[0].ulValueLen * sizeof(CK_BYTE));
    obj_template[0].pValue = value;

    rv = self->p11->C_GetAttributeValue(self->session, key_handle, obj_template, 1);
    if (!check_return_value(rv, "get attribute value")){
    	free(value);
    	return NULL;
    }

    if (obj_template[0].ulValueLen <= 0){
    	PyErr_SetString(IPA_PKCS11NotFound, "Value not found");
    	free(value);
    	return NULL;
    }
    ret = Py_BuildValue("{s:s#}",
    		"value", obj_template[0].pValue, obj_template[0].ulValueLen);
    free(value);
	return ret;
}

/**
 * export RSA public key
 */
static PyObject *
IPA_PKCS11_export_RSA_public_key(IPA_PKCS11* self, CK_OBJECT_HANDLE object)
{
	CK_RV rv;
	PyObject *ret = NULL;

    int pp_len;
    unsigned char *pp = NULL;
    EVP_PKEY *pkey = NULL;
    BIGNUM *e = NULL;
    BIGNUM *n = NULL;
    RSA *rsa = NULL;
    CK_BYTE_PTR modulus = NULL;
    CK_BYTE_PTR exponent = NULL;
    CK_OBJECT_CLASS class = CKO_PUBLIC_KEY;
    CK_KEY_TYPE key_type = CKK_RSA;

    CK_ATTRIBUTE obj_template[] = {
         {CKA_MODULUS, NULL_PTR, 0},
         {CKA_PUBLIC_EXPONENT, NULL_PTR, 0},
         {CKA_CLASS, &class, sizeof(class)},
         {CKA_KEY_TYPE, &key_type, sizeof(key_type)}
    };

    rv = self->p11->C_GetAttributeValue(self->session, object, obj_template, 4);
    if(!check_return_value(rv, "get RSA public key values - prepare"))
    	return NULL;

    /* Set proper size for attributes*/
    modulus = (CK_BYTE_PTR) malloc(obj_template[0].ulValueLen * sizeof(CK_BYTE));
    obj_template[0].pValue = modulus;
    exponent = (CK_BYTE_PTR) malloc(obj_template[1].ulValueLen * sizeof(CK_BYTE));
    obj_template[1].pValue = exponent;

    rv = self->p11->C_GetAttributeValue(self->session, object, obj_template, 4);
    if(!check_return_value(rv, "get RSA public key values"))
    	return NULL;

    /* Check if the key is RSA public key */
    if (class != CKO_PUBLIC_KEY){
    	PyErr_SetString(IPA_PKCS11Error, "export_RSA_public_key: required public key class");
    	return NULL;
    }

    if (key_type != CKK_RSA){
    	PyErr_SetString(IPA_PKCS11Error, "export_RSA_public_key: required RSA key type");
    	return NULL;
    }

    rsa = RSA_new();
    pkey = EVP_PKEY_new();
    n = BN_bin2bn((const unsigned char *) modulus, obj_template[0].ulValueLen * sizeof(CK_BYTE), NULL);
    if( n == NULL ) {
        PyErr_SetString(IPA_PKCS11Error, "export_RSA_public_key: internal error: unable to convert modulus");
        goto final;
    }

    e = BN_bin2bn((const unsigned char *) exponent, obj_template[1].ulValueLen * sizeof(CK_BYTE), NULL);
    if( e == NULL ) {
        PyErr_SetString(IPA_PKCS11Error, "export_RSA_public_key: internal error: unable to convert exponent");
        goto final;
    }

    /* set modulus and exponent */
    rsa->n = n;
    rsa->e = e;

    if (EVP_PKEY_set1_RSA(pkey,rsa) == 0){
        PyErr_SetString(IPA_PKCS11Error, "export_RSA_public_key: internal error: EVP_PKEY_set1_RSA failed");
        goto final;
    }

    pp_len = i2d_PUBKEY(pkey,&pp);
    ret = Py_BuildValue("s#",pp, pp_len);

final:
	if (rsa != NULL) {
		RSA_free(rsa); // this free also 'n' and 'e'
	} else {
		if (n != NULL) BN_free(n);
		if (e != NULL) BN_free(e);
	}

	if (pkey != NULL) EVP_PKEY_free(pkey);
	if (pp != NULL) free(pp);
	return ret;
}

/**
 * Export public key
 *
 * Export public key in SubjectPublicKeyInfo (RFC5280) DER encoded format
 */
static PyObject *
IPA_PKCS11_export_public_key(IPA_PKCS11* self, PyObject *args, PyObject *kwds)
{
	CK_RV rv;
    CK_OBJECT_HANDLE object = 0;
    CK_OBJECT_CLASS class = CKO_PUBLIC_KEY;
    CK_KEY_TYPE key_type = CKK_RSA;
	static char *kwlist[] = {"key_handle", NULL };
	//TODO check long overflow
	if (!PyArg_ParseTupleAndKeywords(args, kwds, "k|", kwlist,
			 &object)){
		return NULL;
	}

    CK_ATTRIBUTE obj_template[] = {
         {CKA_CLASS, &class, sizeof(class)},
         {CKA_KEY_TYPE, &key_type, sizeof(key_type)}
    };

    rv = self->p11->C_GetAttributeValue(self->session, object, obj_template, 2);
    if(!check_return_value(rv, "export_public_key: get RSA public key values"))
    	return NULL;

    if (class != CKO_PUBLIC_KEY){
    	PyErr_SetString(IPA_PKCS11Error, "export_public_key: required public key class");
    	return NULL;
    }

    switch (key_type){
    case CKK_RSA:
    	return IPA_PKCS11_export_RSA_public_key(self, object);
    	break;
    default:
    	PyErr_SetString(IPA_PKCS11Error, "export_public_key: unsupported key type");
    }

    return NULL;
}

/**
 * Import RSA public key
 *
 */
static PyObject *
IPA_PKCS11_import_RSA_public_key(IPA_PKCS11* self, CK_UTF8CHAR *label, Py_ssize_t label_length,
		CK_BYTE *id, Py_ssize_t id_length, EVP_PKEY *pkey, CK_BBOOL* cka_copyable,
		CK_BBOOL* cka_derive, CK_BBOOL* cka_encrypt,
		CK_BBOOL* cka_modifiable, CK_BBOOL* cka_private, CK_BBOOL* cka_trusted,
		CK_BBOOL* cka_verify, CK_BBOOL* cka_verify_recover, CK_BBOOL* cka_wrap)
{
    CK_RV rv;
    CK_OBJECT_CLASS class = CKO_PUBLIC_KEY;
    CK_KEY_TYPE keyType = CKK_RSA;
    CK_BBOOL *cka_token = &true;
    RSA *rsa = NULL;
	CK_BYTE_PTR modulus = NULL;
	int modulus_len = 0;
	CK_BYTE_PTR exponent = NULL;
	int exponent_len = 0;

	CK_ATTRIBUTE template[] = {
		{CKA_ID, id, id_length},
		{CKA_CLASS, &class, sizeof(class)},
		{CKA_KEY_TYPE, &keyType, sizeof(keyType)},
		{CKA_TOKEN, cka_token, sizeof(CK_BBOOL)},
		{CKA_LABEL, label, label_length},
		{CKA_MODULUS, modulus, modulus_len},
		{CKA_PUBLIC_EXPONENT, exponent, exponent_len},
        //{CKA_COPYABLE, cka_copyable, sizeof(CK_BBOOL)}, //TODO Softhsm doesn't support it
        {CKA_DERIVE, cka_derive, sizeof(CK_BBOOL)},
        {CKA_ENCRYPT, cka_encrypt, sizeof(CK_BBOOL)},
        {CKA_MODIFIABLE, cka_modifiable, sizeof(CK_BBOOL)},
        {CKA_PRIVATE, cka_private, sizeof(CK_BBOOL)},
        {CKA_TRUSTED, cka_trusted, sizeof(CK_BBOOL)},
        {CKA_VERIFY, cka_verify, sizeof(CK_BBOOL)},
        {CKA_VERIFY_RECOVER, cka_verify_recover, sizeof(CK_BBOOL)},
        {CKA_WRAP, cka_wrap, sizeof(CK_BBOOL)},
		};
    CK_OBJECT_HANDLE object;

	if (pkey->type != EVP_PKEY_RSA){
		PyErr_SetString(IPA_PKCS11Error, "Required RSA public key");
		return NULL;
	}

    rsa = EVP_PKEY_get1_RSA(pkey);
    if (rsa == NULL){
    	PyErr_SetString(IPA_PKCS11Error, "import_RSA_public_key: EVP_PKEY_get1_RSA error");
    	free(pkey);
    	return NULL;
    }

    /* convert BIGNUM to binary array */
    modulus = (CK_BYTE_PTR) malloc(BN_num_bytes(rsa->n));
    modulus_len = BN_bn2bin(rsa->n, (unsigned char *) modulus);
    if(modulus == NULL){
    	PyErr_SetString(IPA_PKCS11Error, "import_RSA_public_key: BN_bn2bin modulus error");
    	//TODO free
    	return NULL;
    }

    exponent = (CK_BYTE_PTR) malloc(BN_num_bytes(rsa->e));
    exponent_len = BN_bn2bin(rsa->e, (unsigned char *) exponent);
    if(exponent == NULL){
    	PyErr_SetString(IPA_PKCS11Error, "import_RSA_public_key: BN_bn2bin exponent error");
    	//TODO free
    	return NULL;
    }

    rv = self->p11->C_CreateObject(self->session, template,
    		sizeof(template)/sizeof(CK_ATTRIBUTE), &object);
    if(!check_return_value(rv, "create public key object"))
    	return NULL;

    if (rsa != NULL) RSA_free(rsa);

	return PyLong_FromUnsignedLong(object);
}

/**
 * Import RSA public key
 *
 */
static PyObject *
IPA_PKCS11_import_public_key(IPA_PKCS11* self, PyObject *args, PyObject *kwds){
    int r;
    PyObject *ret = NULL;
    PyObject *label_unicode = NULL;
    CK_BYTE *id = NULL;
    CK_BYTE *data = NULL;
    CK_UTF8CHAR *label = NULL;
    Py_ssize_t id_length = 0;
    Py_ssize_t data_length = 0;
    Py_ssize_t label_length = 0;
    EVP_PKEY *pkey = NULL;

    PyObj2Bool_mapping_t attrs_pub[] = {
        {NULL, &true}, //pub_en_cka_copyable
        {NULL, &false}, //pub_en_cka_derive
        {NULL, &false}, //pub_en_cka_encrypt
        {NULL, &true}, //pub_en_cka_modifiable
        {NULL, &true}, //pub_en_cka_private
        {NULL, &false}, //pub_en_cka_trusted
        {NULL, &true}, //pub_en_cka_verify
        {NULL, &true}, //pub_en_cka_verify_recover
        {NULL, &false}, //pub_en_cka_wrap
    };

    static char *kwlist[] = {"label", "id", "data",
    		/* public key attributes */
    		"cka_copyable", "cka_derive", "cka_encrypt",
    		"cka_modifiable", "cka_private", "cka_trusted", "cka_verify",
    		"cka_verify_recover", "cka_wrap" , NULL };
	if (!PyArg_ParseTupleAndKeywords(args, kwds, "Us#s#|OOOOOOOOO", kwlist,
			&label_unicode, &id, &id_length, &data, &data_length,
			/* public key attributes */
            &attrs_pub[pub_en_cka_copyable].py_obj,
            &attrs_pub[pub_en_cka_derive].py_obj,
            &attrs_pub[pub_en_cka_encrypt].py_obj,
            &attrs_pub[pub_en_cka_modifiable].py_obj,
            &attrs_pub[pub_en_cka_private].py_obj,
            &attrs_pub[pub_en_cka_trusted].py_obj,
            &attrs_pub[pub_en_cka_verify].py_obj,
            &attrs_pub[pub_en_cka_verify_recover].py_obj,
            &attrs_pub[pub_en_cka_wrap].py_obj)){
		return NULL;
	}
	Py_XINCREF(label_unicode);
	label = (unsigned char*) unicode_to_char_array(label_unicode, &label_length);
	Py_XDECREF(label_unicode);


    r = _id_label_exists(self, id, id_length, label, label_length, CKO_PUBLIC_KEY);
    if (r == 1){
    	PyErr_SetString(IPA_PKCS11DuplicationError,
    			"Public key with same ID and LABEL already exists");
    	return NULL;
    } else if (r == -1){
    	return NULL;
    }

    /* Process keyword boolean arguments */
    convert_py2bool(attrs_pub,
                    sizeof(attrs_pub)/sizeof(PyObj2Bool_mapping_t));

	/* decode from ASN1 DER */
    pkey = d2i_PUBKEY(NULL, (const unsigned char **) &data, data_length);
    if(pkey == NULL){
    	PyErr_SetString(IPA_PKCS11Error, "import_public_key: d2i_PUBKEY error");
    	return NULL;
    }
	switch(pkey->type){
	case EVP_PKEY_RSA:
		ret = IPA_PKCS11_import_RSA_public_key(self, label, label_length,
			id, id_length, pkey,
			attrs_pub[pub_en_cka_copyable].bool,
			attrs_pub[pub_en_cka_derive].bool,
			attrs_pub[pub_en_cka_encrypt].bool,
			attrs_pub[pub_en_cka_modifiable].bool,
			attrs_pub[pub_en_cka_private].bool,
			attrs_pub[pub_en_cka_trusted].bool,
			attrs_pub[pub_en_cka_verify].bool,
			attrs_pub[pub_en_cka_verify_recover].bool,
			attrs_pub[pub_en_cka_wrap].bool);
		break;
	case EVP_PKEY_DSA:
		ret = NULL;
		PyErr_SetString(IPA_PKCS11Error, "DSA is not supported");
		break;
	case EVP_PKEY_EC:
		ret = NULL;
		PyErr_SetString(IPA_PKCS11Error, "EC is not supported");
		break;
	default:
		ret = NULL;
		PyErr_SetString(IPA_PKCS11Error, "Unsupported key type");
	}
    if (pkey != NULL) EVP_PKEY_free(pkey);
	return ret;
}

/**
 * Export wrapped key
 *
 */
static PyObject *
IPA_PKCS11_export_wrapped_key(IPA_PKCS11* self, PyObject *args, PyObject *kwds)
{
	CK_RV rv;
    CK_OBJECT_HANDLE object_key = 0;
    CK_OBJECT_HANDLE object_wrapping_key = 0;
	CK_BYTE_PTR wrapped_key = NULL;
	CK_ULONG wrapped_key_len = 0;
	CK_MECHANISM wrapping_mech = {CKM_RSA_PKCS, NULL, 0};
	CK_MECHANISM_TYPE wrapping_mech_type= CKM_RSA_PKCS;
	/* currently we don't support parameter in mechanism */

	static char *kwlist[] = {"key", "wrapping_key", "wrapping_mech", NULL };
	//TODO check long overflow
	//TODO export method
	if (!PyArg_ParseTupleAndKeywords(args, kwds, "kkk|", kwlist,
			 &object_key, &object_wrapping_key, &wrapping_mech_type)){
		return NULL;
	}
	wrapping_mech.mechanism = wrapping_mech_type;

    rv = self->p11->C_WrapKey(self->session, &wrapping_mech, object_wrapping_key,
   		 object_key, NULL, &wrapped_key_len);
    if(!check_return_value(rv, "key wrapping: get buffer length"))
   	 return 0;
    wrapped_key = malloc(wrapped_key_len);
    if (wrapped_key == NULL) {
	     rv = CKR_HOST_MEMORY;
	     check_return_value(rv, "key wrapping: buffer allocation");
	     return 0;
    }
    rv = self->p11->C_WrapKey(self->session, &wrapping_mech, object_wrapping_key,
        object_key, wrapped_key, &wrapped_key_len);
    if(!check_return_value(rv, "key wrapping: wrapping"))
        return NULL;


	return Py_BuildValue("s#", wrapped_key, wrapped_key_len);

}

/**
 * Import wrapped secret key
 *
 */
static PyObject *
IPA_PKCS11_import_wrapped_secret_key(IPA_PKCS11* self, PyObject *args, PyObject *kwds)
{
	CK_RV rv;
	int r;
	CK_BYTE_PTR wrapped_key = NULL;
	CK_ULONG wrapped_key_len = 0;
	CK_ULONG unwrapping_key_object = 0;
	CK_OBJECT_HANDLE unwrapped_key_object = 0;
	PyObject *label_unicode = NULL;
    CK_BYTE *id = NULL;
    CK_UTF8CHAR *label = NULL;
    Py_ssize_t id_length = 0;
    Py_ssize_t label_length = 0;
	CK_MECHANISM wrapping_mech = {CKM_RSA_PKCS, NULL, 0};
	CK_OBJECT_CLASS key_class = CKO_SECRET_KEY;
	CK_KEY_TYPE key_type = CKK_RSA;

    PyObj2Bool_mapping_t attrs[] = {
        {NULL, &true}, //sec_en_cka_copyable
        {NULL, &false}, //sec_en_cka_decrypt
        {NULL, &false}, //sec_en_cka_derive
        {NULL, &false}, //sec_en_cka_encrypt
        {NULL, &true}, //sec_en_cka_extractable
        {NULL, &true}, //sec_en_cka_modifiable
        {NULL, &true}, //sec_en_cka_private
        {NULL, &true}, //sec_en_cka_sensitive
        {NULL, &false}, //sec_en_cka_sign
        {NULL, &true}, //sec_en_cka_unwrap
        {NULL, &false}, //sec_en_cka_verify
        {NULL, &true}, //sec_en_cka_wrap
        {NULL, &false} //sec_en_cka_wrap_with_trusted
    };

    static char *kwlist[] = {"label", "id", "data", "unwrapping_key", "wrapping_mech",
    		"key_type",
    		// secret key attrs
    		"cka_copyable",
			"cka_decrypt", "cka_derive", "cka_encrypt", "cka_extractable",
			"cka_modifiable", "cka_private", "cka_sensitive", "cka_sign",
			"cka_unwrap", "cka_verify", "cka_wrap", "cka_wrap_with_trusted", NULL };
	if (!PyArg_ParseTupleAndKeywords(args, kwds, "Us#s#kkk|OOOOOOOOOOOOO",
			kwlist, &label_unicode, &id, &id_length,
			&wrapped_key, &wrapped_key_len, &unwrapping_key_object,
			&wrapping_mech.mechanism, &key_type,
			// secret key attrs
			&attrs[sec_en_cka_copyable].py_obj,
			&attrs[sec_en_cka_decrypt].py_obj,
			&attrs[sec_en_cka_derive].py_obj,
			&attrs[sec_en_cka_encrypt].py_obj,
			&attrs[sec_en_cka_extractable].py_obj,
			&attrs[sec_en_cka_modifiable].py_obj,
			&attrs[sec_en_cka_private].py_obj,
			&attrs[sec_en_cka_sensitive].py_obj,
			&attrs[sec_en_cka_sign].py_obj,
			&attrs[sec_en_cka_unwrap].py_obj,
			&attrs[sec_en_cka_verify].py_obj,
			&attrs[sec_en_cka_wrap].py_obj,
			&attrs[sec_en_cka_wrap_with_trusted].py_obj)){
		return NULL;
	}
	Py_XINCREF(label_unicode);
	label = (unsigned char*) unicode_to_char_array(label_unicode, &label_length); //TODO verify signed/unsigned
	Py_XDECREF(label_unicode);

    r = _id_label_exists(self, id, id_length, label, label_length, key_class);
    if (r == 1){
    	PyErr_SetString(IPA_PKCS11DuplicationError,
    			"Secret key with same ID and LABEL already exists");
    	return NULL;
    } else if (r == -1){
    	return NULL;
    }

    /* Process keyword boolean arguments */
    convert_py2bool(attrs,
                    sizeof(attrs)/sizeof(PyObj2Bool_mapping_t));

    CK_ATTRIBUTE template[] = {
         {CKA_CLASS, &key_class, sizeof(key_class)},
         {CKA_KEY_TYPE, &key_type, sizeof(key_type)},
         {CKA_ID, id, id_length},
         {CKA_LABEL, label, label_length},
         {CKA_TOKEN, &true, sizeof(CK_BBOOL)},
         //{CKA_COPYABLE, attrs[sec_en_cka_copyable].bool, sizeof(CK_BBOOL)}, //TODO Softhsm doesn't support it
         {CKA_DECRYPT, attrs[sec_en_cka_decrypt].bool, sizeof(CK_BBOOL)},
         {CKA_DERIVE, attrs[sec_en_cka_derive].bool, sizeof(CK_BBOOL)},
         {CKA_ENCRYPT, attrs[sec_en_cka_encrypt].bool, sizeof(CK_BBOOL)},
         {CKA_EXTRACTABLE, attrs[sec_en_cka_extractable].bool, sizeof(CK_BBOOL)},
         {CKA_MODIFIABLE, attrs[sec_en_cka_modifiable].bool, sizeof(CK_BBOOL)},
         {CKA_PRIVATE, attrs[sec_en_cka_private].bool, sizeof(CK_BBOOL)},
         {CKA_SENSITIVE, attrs[sec_en_cka_sensitive].bool, sizeof(CK_BBOOL)},
         {CKA_SIGN, attrs[sec_en_cka_sign].bool, sizeof(CK_BBOOL)},
         {CKA_UNWRAP, attrs[sec_en_cka_unwrap].bool, sizeof(CK_BBOOL)},
         {CKA_VERIFY, attrs[sec_en_cka_verify].bool, sizeof(CK_BBOOL)},
         {CKA_WRAP, attrs[sec_en_cka_wrap].bool, sizeof(CK_BBOOL)},
         {CKA_WRAP_WITH_TRUSTED, attrs[sec_en_cka_wrap_with_trusted].bool, sizeof(CK_BBOOL)}
    };

    rv = self->p11->C_UnwrapKey(self->session, &wrapping_mech, unwrapping_key_object,
    				wrapped_key, wrapped_key_len, template,
    				sizeof(template)/sizeof(CK_ATTRIBUTE), &unwrapped_key_object);
    if(!check_return_value(rv, "import_wrapped_key: key unwrapping")){
    	return NULL;
    }

    return PyLong_FromUnsignedLong(unwrapped_key_object);

}

/**
 * Import wrapped private key
 *
 */
static PyObject *
IPA_PKCS11_import_wrapped_private_key(IPA_PKCS11* self, PyObject *args, PyObject *kwds)
{
	CK_RV rv;
	int r;
	CK_BYTE_PTR wrapped_key = NULL;
	CK_ULONG wrapped_key_len = 0;
	CK_ULONG unwrapping_key_object = 0;
	CK_OBJECT_HANDLE unwrapped_key_object = 0;
	PyObject *label_unicode = NULL;
    CK_BYTE *id = NULL;
    CK_UTF8CHAR *label = NULL;
    Py_ssize_t id_length = 0;
    Py_ssize_t label_length = 0;
	CK_MECHANISM wrapping_mech = {CKM_RSA_PKCS, NULL, 0};
	CK_OBJECT_CLASS key_class = CKO_SECRET_KEY;
	CK_KEY_TYPE key_type = CKK_RSA;

    PyObj2Bool_mapping_t attrs_priv[] = {
        {NULL, &false}, //priv_en_cka_always_authenticate
        {NULL, &true}, //priv_en_cka_copyable
        {NULL, &false}, //priv_en_cka_decrypt
        {NULL, &false}, //priv_en_cka_derive
        {NULL, &true}, //priv_en_cka_extractable
        {NULL, &true}, //priv_en_cka_modifiable
        {NULL, &true}, //priv_en_cka_private
        {NULL, &true}, //priv_en_cka_sensitive
        {NULL, &true}, //priv_en_cka_sign
        {NULL, &true}, //priv_en_cka_sign_recover
        {NULL, &false}, //priv_en_cka_unwrap
        {NULL, &false} //priv_en_cka_wrap_with_trusted
    };

    static char *kwlist[] = {"label", "id", "data", "unwrapping_key", "wrapping_mech",
    		"key_type",
    		// private key attrs
    		"cka_always_authenticate", "cka_copyable",
			"cka_decrypt", "cka_derive", "cka_extractable",
			"cka_modifiable", "cka_private", "cka_sensitive", "cka_sign",
			"cka_sign_recover", "cka_unwrap", "cka_wrap_with_trusted", NULL };
	if (!PyArg_ParseTupleAndKeywords(args, kwds, "Us#s#kkk|OOOOOOOOOOOO",
			kwlist, &label_unicode, &id, &id_length,
			&wrapped_key, &wrapped_key_len, &unwrapping_key_object,
			&wrapping_mech.mechanism, &key_type,
			// private key attrs
            &attrs_priv[priv_en_cka_always_authenticate].py_obj,
            &attrs_priv[priv_en_cka_copyable].py_obj,
            &attrs_priv[priv_en_cka_decrypt].py_obj,
            &attrs_priv[priv_en_cka_derive].py_obj,
            &attrs_priv[priv_en_cka_extractable].py_obj,
            &attrs_priv[priv_en_cka_modifiable].py_obj,
            &attrs_priv[priv_en_cka_private].py_obj,
            &attrs_priv[priv_en_cka_sensitive].py_obj,
            &attrs_priv[priv_en_cka_sign].py_obj,
            &attrs_priv[priv_en_cka_sign_recover].py_obj,
            &attrs_priv[priv_en_cka_unwrap].py_obj,
            &attrs_priv[priv_en_cka_wrap_with_trusted].py_obj)){
		return NULL;
	}
	Py_XINCREF(label_unicode);
	label = (unsigned char*) unicode_to_char_array(label_unicode, &label_length); //TODO verify signed/unsigned
	Py_XDECREF(label_unicode);

    r = _id_label_exists(self, id, id_length, label, label_length, CKO_SECRET_KEY);
    if (r == 1){
    	PyErr_SetString(IPA_PKCS11DuplicationError,
    			"Secret key with same ID and LABEL already exists");
    	return NULL;
    } else if (r == -1){
    	return NULL;
    }

    /* Process keyword boolean arguments */
    convert_py2bool(attrs_priv,
                    sizeof(attrs_priv)/sizeof(PyObj2Bool_mapping_t));

    CK_ATTRIBUTE template[] = {
         {CKA_CLASS, &key_class, sizeof(key_class)},
         {CKA_KEY_TYPE, &key_type, sizeof(key_type)},
         {CKA_ID, id, id_length},
         {CKA_LABEL, label, label_length},
         {CKA_TOKEN, &true, sizeof(CK_BBOOL)},
         {CKA_ALWAYS_AUTHENTICATE, attrs_priv[priv_en_cka_always_authenticate].bool, sizeof(CK_BBOOL)},
         //{CKA_COPYABLE, attrs_priv[priv_en_cka_copyable].bool, sizeof(CK_BBOOL)}, //TODO Softhsm doesn't support it
         {CKA_DECRYPT, attrs_priv[priv_en_cka_decrypt].bool, sizeof(CK_BBOOL)},
         {CKA_DERIVE, attrs_priv[priv_en_cka_derive].bool, sizeof(CK_BBOOL)},
         {CKA_EXTRACTABLE, attrs_priv[priv_en_cka_extractable].bool, sizeof(CK_BBOOL)},
         {CKA_MODIFIABLE, attrs_priv[priv_en_cka_modifiable].bool, sizeof(CK_BBOOL)},
         {CKA_PRIVATE, attrs_priv[priv_en_cka_private].bool, sizeof(CK_BBOOL)},
         {CKA_SENSITIVE, attrs_priv[priv_en_cka_sensitive].bool, sizeof(CK_BBOOL)},
         {CKA_SIGN, attrs_priv[priv_en_cka_sign].bool, sizeof(CK_BBOOL)},
         {CKA_SIGN_RECOVER, attrs_priv[priv_en_cka_sign].bool, sizeof(CK_BBOOL)},
         {CKA_UNWRAP, attrs_priv[priv_en_cka_unwrap].bool, sizeof(CK_BBOOL)},
         {CKA_WRAP_WITH_TRUSTED, attrs_priv[priv_en_cka_wrap_with_trusted].bool, sizeof(CK_BBOOL)}
    };

    rv = self->p11->C_UnwrapKey(self->session, &wrapping_mech, unwrapping_key_object,
    				wrapped_key, wrapped_key_len, template,
    				sizeof(template)/sizeof(CK_ATTRIBUTE), &unwrapped_key_object);
    if(!check_return_value(rv, "import_wrapped_key: key unwrapping")){
    	return NULL;
    }

    return PyLong_FromUnsignedLong(unwrapped_key_object);

}

/*
 * Set object attributes
 */
static PyObject *
IPA_PKCS11_set_attribute(IPA_PKCS11* self, PyObject *args, PyObject *kwds){
	PyObject *ret = Py_None;
	PyObject *value = NULL;
    CK_ULONG object = 0;
    unsigned long attr = 0;
	CK_ATTRIBUTE attribute;
	CK_RV rv;
	Py_ssize_t len = 0;

    static char *kwlist[] = {"key_object", "attr", "value", NULL };
	if (!PyArg_ParseTupleAndKeywords(args, kwds, "kkO|", kwlist, &object,
			&attr, &value)){
		return NULL;
	}
	Py_XINCREF(value);
	attribute.type = attr;
	switch(attr){
	case CKA_ALWAYS_AUTHENTICATE:
	case CKA_ALWAYS_SENSITIVE:
	case CKA_COPYABLE:
	case CKA_ENCRYPT:
	case CKA_EXTRACTABLE:
	case CKA_DECRYPT:
	case CKA_DERIVE:
	case CKA_LOCAL:
	case CKA_MODIFIABLE:
	case CKA_NEVER_EXTRACTABLE:
	case CKA_PRIVATE:
	case CKA_SENSITIVE:
	case CKA_SIGN:
	case CKA_SIGN_RECOVER:
	case CKA_TOKEN:
	case CKA_TRUSTED:
	case CKA_UNWRAP:
	case CKA_VERIFY:
	case CKA_VERIFY_RECOVER:
	case CKA_WRAP:
	case CKA_WRAP_WITH_TRUSTED:
		attribute.pValue = PyObject_IsTrue(value) ? &true : &false;
		attribute.ulValueLen = sizeof(CK_BBOOL);
		break;
	case CKA_ID:
		if(!PyString_Check(value)){
			PyErr_SetString(IPA_PKCS11Error, "String value expected");
			ret = NULL;
			goto final;
		}
		if (PyString_AsStringAndSize(value,
				(char **) &attribute.pValue, &attribute.ulValueLen) == -1){
			ret = NULL;
			goto final;
		}
		break;
	case CKA_LABEL:
		if(!PyUnicode_Check(value)){
			PyErr_SetString(IPA_PKCS11Error, "Unicode value expected");
			ret = NULL;
			goto final;
		}
		attribute.pValue = unicode_to_char_array(value, &len);
		/* check for conversion error */
		if (attribute.pValue == NULL){
			ret = NULL;
			goto final;
		}
		attribute.ulValueLen = len;
		break;
	case CKA_KEY_TYPE:
		if(!PyInt_Check(value)){
			PyErr_SetString(IPA_PKCS11Error, "Integer value expected");
			ret = NULL;
			goto final;
		}
		unsigned long lv = PyInt_AsUnsignedLongMask(value);
		attribute.pValue = &lv;
		attribute.ulValueLen = sizeof(unsigned long);
		break;
	default:
		ret = NULL;
		PyErr_SetString(IPA_PKCS11Error, "Unknown attribute");
		goto final;
	}

	CK_ATTRIBUTE template[] = {attribute};

	rv = self->p11->C_SetAttributeValue(self->session, object, template, 1);
    if(!check_return_value(rv, "set_attribute"))
    	ret = NULL;
final:
	Py_XDECREF(value);
	return ret;
}

/*
 * Get object attributes
 */
static PyObject *
IPA_PKCS11_get_attribute(IPA_PKCS11* self, PyObject *args, PyObject *kwds){
	PyObject *ret = NULL;
	void *value = NULL;
    CK_ULONG object = 0;
    unsigned long attr = 0;
	CK_ATTRIBUTE attribute;
	CK_RV rv;

    static char *kwlist[] = {"key_object", "attr", NULL };
	if (!PyArg_ParseTupleAndKeywords(args, kwds, "kk|", kwlist, &object,
			&attr)){
		return NULL;
	}

	attribute.type = attr;
	attribute.pValue = NULL_PTR;
	attribute.ulValueLen = 0;
	CK_ATTRIBUTE template[] = {attribute};

	rv = self->p11->C_GetAttributeValue(self->session, object, template, 1);
	// attribute doesn't exists
	if (rv == CKR_ATTRIBUTE_TYPE_INVALID || template[0].ulValueLen == (unsigned long) -1){
		PyErr_SetString(IPA_PKCS11NotFound, "attribute does not exist");
		ret = NULL;
		goto final;
	}
    if(!check_return_value(rv, "get_attribute init")){
    	ret = NULL;
    	goto final;
    }
    value = malloc(template[0].ulValueLen);
    template[0].pValue = value;

	rv = self->p11->C_GetAttributeValue(self->session, object, template, 1);
    if(!check_return_value(rv, "get_attribute")){
    	ret = NULL;
    	goto final;
    }

	switch(attr){
	case CKA_ALWAYS_AUTHENTICATE:
	case CKA_ALWAYS_SENSITIVE:
	case CKA_COPYABLE:
	case CKA_ENCRYPT:
	case CKA_EXTRACTABLE:
	case CKA_DECRYPT:
	case CKA_DERIVE:
	case CKA_LOCAL:
	case CKA_MODIFIABLE:
	case CKA_NEVER_EXTRACTABLE:
	case CKA_PRIVATE:
	case CKA_SENSITIVE:
	case CKA_SIGN:
	case CKA_SIGN_RECOVER:
	case CKA_TOKEN:
	case CKA_TRUSTED:
	case CKA_UNWRAP:
	case CKA_VERIFY:
	case CKA_VERIFY_RECOVER:
	case CKA_WRAP:
	case CKA_WRAP_WITH_TRUSTED:
		/* booleans */
		ret = PyBool_FromLong(*(CK_BBOOL*)value);
		break;
	case CKA_LABEL:
		/* unicode string */
		ret = char_array_to_unicode(value, template[0].ulValueLen);
		break;
	case CKA_ID:
		/* byte arrays */
		ret = Py_BuildValue("s#", value, template[0].ulValueLen);
		break;
	case CKA_KEY_TYPE:
		/* unsigned long */
		ret = Py_BuildValue("k", (unsigned long *) value);
		break;
	default:
		ret = NULL;
		PyErr_SetString(IPA_PKCS11Error, "Unknown attribute");
		goto final;
	}

final:
	if(value != NULL) free(value);
	return ret;
}

static PyMethodDef IPA_PKCS11_methods[] = {
		{ "initialize",
		(PyCFunction) IPA_PKCS11_initialize, METH_VARARGS,
		"Inicialize pkcs11 library" },
		{ "finalize",
		(PyCFunction) IPA_PKCS11_finalize, METH_NOARGS,
		"Finalize operations with pkcs11 library" },
		{ "generate_master_key",
		(PyCFunction) IPA_PKCS11_generate_master_key, METH_VARARGS|METH_KEYWORDS,
		"Generate master key" },
		{ "generate_replica_key_pair",
		(PyCFunction) IPA_PKCS11_generate_replica_key_pair, METH_VARARGS|METH_KEYWORDS,
		"Generate replica key pair" },
		{ "get_key_handle",
		(PyCFunction) IPA_PKCS11_get_key_handle, METH_VARARGS|METH_KEYWORDS,
		"Get key" },
		{ "find_keys",
		(PyCFunction) IPA_PKCS11_find_keys, METH_VARARGS|METH_KEYWORDS,
		"Find keys" },
		{ "delete_key",
		(PyCFunction) IPA_PKCS11_delete_key, METH_VARARGS|METH_KEYWORDS,
		"Delete key" },
		{ "export_secret_key", //TODO deprecated, delete it
		(PyCFunction) IPA_PKCS11_export_secret_key, METH_VARARGS|METH_KEYWORDS,
		"Export secret key" },
		{ "export_public_key",
		(PyCFunction) IPA_PKCS11_export_public_key, METH_VARARGS|METH_KEYWORDS,
		"Export public key" },
		{ "import_public_key",
		(PyCFunction) IPA_PKCS11_import_public_key, METH_VARARGS|METH_KEYWORDS,
		"Import public key" },
		{ "export_wrapped_key",
		(PyCFunction) IPA_PKCS11_export_wrapped_key, METH_VARARGS|METH_KEYWORDS,
		"Export wrapped private key" },
		{ "import_wrapped_secret_key",
		(PyCFunction) IPA_PKCS11_import_wrapped_secret_key, METH_VARARGS|METH_KEYWORDS,
		"Import wrapped secret key" },
		{ "import_wrapped_private_key",
		(PyCFunction) IPA_PKCS11_import_wrapped_private_key, METH_VARARGS|METH_KEYWORDS,
		"Import wrapped private key" },
		{ "set_attribute",
		(PyCFunction) IPA_PKCS11_set_attribute, METH_VARARGS|METH_KEYWORDS,
		"Set attribute" },
		{ "get_attribute",
		(PyCFunction) IPA_PKCS11_get_attribute, METH_VARARGS|METH_KEYWORDS,
		"Get attribute" },
		{ NULL } /* Sentinel */
};

static PyTypeObject IPA_PKCS11Type = {
	PyObject_HEAD_INIT(NULL)
	0, /*ob_size*/
	"ipapkcs11.IPA_PKCS11", /*tp_name*/
	sizeof(IPA_PKCS11), /*tp_basicsize*/
	0, /*tp_itemsize*/
	(destructor)IPA_PKCS11_dealloc, /*tp_dealloc*/
	0, /*tp_print*/
	0, /*tp_getattr*/
	0, /*tp_setattr*/
	0, /*tp_compare*/
	0, /*tp_repr*/
	0, /*tp_as_number*/
	0, /*tp_as_sequence*/
	0, /*tp_as_mapping*/
	0, /*tp_hash */
	0, /*tp_call*/
	0, /*tp_str*/
	0, /*tp_getattro*/
	0, /*tp_setattro*/
	0, /*tp_as_buffer*/
	Py_TPFLAGS_DEFAULT | Py_TPFLAGS_BASETYPE, /*tp_flags*/
	"IPA_PKCS11 objects", /* tp_doc */
	0, /* tp_traverse */
	0, /* tp_clear */
	0, /* tp_richcompare */
	0, /* tp_weaklistoffset */
	0, /* tp_iter */
	0, /* tp_iternext */
	IPA_PKCS11_methods, /* tp_methods */
	IPA_PKCS11_members, /* tp_members */
	0, /* tp_getset */
	0, /* tp_base */
	0, /* tp_dict */
	0, /* tp_descr_get */
	0, /* tp_descr_set */
	0, /* tp_dictoffset */
	(initproc)IPA_PKCS11_init, /* tp_init */
	0, /* tp_alloc */
	IPA_PKCS11_new, /* tp_new */
};

static PyMethodDef module_methods[] = { { NULL } /* Sentinel */
};

#ifndef PyMODINIT_FUNC	/* declarations for DLL import/export */
#define PyMODINIT_FUNC void
#endif
PyMODINIT_FUNC initipapkcs11(void) {
	PyObject* m;

	if (PyType_Ready(&IPA_PKCS11Type) < 0)
		return;

	/*
	 * Setting up ipa_pkcs11 module
	 */
	m = Py_InitModule3("ipapkcs11", module_methods,
			"Example module that creates an extension type.");

	if (m == NULL)
		return;

	/*
	 * Setting up IPA_PKCS11
	 */
	Py_INCREF(&IPA_PKCS11Type);
	PyModule_AddObject(m, "IPA_PKCS11", (PyObject *) &IPA_PKCS11Type);

	/*
	 * Setting up IPA_PKCS11 Exceptions
	 */
	IPA_PKCS11Exception = PyErr_NewException("IPA_PKCS11.Exception", NULL, NULL);
	Py_INCREF(IPA_PKCS11Exception);
	PyModule_AddObject(m, "Exception", IPA_PKCS11Exception);

	IPA_PKCS11Error = PyErr_NewException("IPA_PKCS11.Error", IPA_PKCS11Exception, NULL);
	Py_INCREF(IPA_PKCS11Error);
	PyModule_AddObject(m, "Error", IPA_PKCS11Error);

	IPA_PKCS11NotFound = PyErr_NewException("IPA_PKCS11.NotFound", IPA_PKCS11Exception, NULL);
	Py_INCREF(IPA_PKCS11NotFound);
	PyModule_AddObject(m, "NotFound", IPA_PKCS11NotFound);

	IPA_PKCS11DuplicationError = PyErr_NewException("IPA_PKCS11.DuplicationError", IPA_PKCS11Exception, NULL);
	Py_INCREF(IPA_PKCS11DuplicationError);
	PyModule_AddObject(m, "DuplicationError", IPA_PKCS11DuplicationError);

	/**
	 * Setting up module attributes
	 */

	/* Key Classes*/
	PyObject *IPA_PKCS11_CLASS_PUBKEY_obj = PyInt_FromLong(CKO_PUBLIC_KEY);
	PyObject_SetAttrString(m, "KEY_CLASS_PUBLIC_KEY", IPA_PKCS11_CLASS_PUBKEY_obj);
	Py_XDECREF(IPA_PKCS11_CLASS_PUBKEY_obj);

	PyObject *IPA_PKCS11_CLASS_PRIVKEY_obj = PyInt_FromLong(CKO_PRIVATE_KEY);
	PyObject_SetAttrString(m, "KEY_CLASS_PRIVATE_KEY", IPA_PKCS11_CLASS_PRIVKEY_obj);
	Py_XDECREF(IPA_PKCS11_CLASS_PRIVKEY_obj);

	PyObject *IPA_PKCS11_CLASS_SECRETKEY_obj = PyInt_FromLong(CKO_SECRET_KEY);
	PyObject_SetAttrString(m, "KEY_CLASS_SECRET_KEY", IPA_PKCS11_CLASS_SECRETKEY_obj);
	Py_XDECREF(IPA_PKCS11_CLASS_SECRETKEY_obj);

	/* Key types*/
	PyObject *IPA_PKCS11_KEY_TYPE_RSA_obj = PyInt_FromLong(CKK_RSA);
	PyObject_SetAttrString(m, "KEY_TYPE_RSA", IPA_PKCS11_KEY_TYPE_RSA_obj);
	Py_XDECREF(IPA_PKCS11_KEY_TYPE_RSA_obj);

	PyObject *IPA_PKCS11_KEY_TYPE_AES_obj = PyInt_FromLong(CKK_AES);
	PyObject_SetAttrString(m, "KEY_TYPE_AES", IPA_PKCS11_KEY_TYPE_AES_obj);
	Py_XDECREF(IPA_PKCS11_KEY_TYPE_AES_obj);

	/* Wrapping mech type*/
	PyObject *IPA_PKCS11_MECH_RSA_PKCS_obj = PyInt_FromLong(CKM_RSA_PKCS);
	PyObject_SetAttrString(m, "MECH_RSA_PKCS", IPA_PKCS11_MECH_RSA_PKCS_obj);
	Py_XDECREF(IPA_PKCS11_MECH_RSA_PKCS_obj);

	PyObject *IPA_PKCS11_MECH_RSA_PKCS_OAEP_obj = PyInt_FromLong(CKM_RSA_PKCS_OAEP);
	PyObject_SetAttrString(m, "MECH_RSA_PKCS_OAEP", IPA_PKCS11_MECH_RSA_PKCS_OAEP_obj);
	Py_XDECREF(IPA_PKCS11_MECH_RSA_PKCS_OAEP_obj);

	PyObject *IPA_PKCS11_MECH_AES_KEY_WRAP_obj = PyInt_FromLong(CKM_AES_KEY_WRAP);
	PyObject_SetAttrString(m, "MECH_AES_KEY_WRAP", IPA_PKCS11_MECH_AES_KEY_WRAP_obj);
	Py_XDECREF(IPA_PKCS11_MECH_AES_KEY_WRAP_obj);

	PyObject *IPA_PKCS11_MECH_AES_KEY_WRAP_PAD_obj = PyInt_FromLong(CKM_AES_KEY_WRAP_PAD);
	PyObject_SetAttrString(m, "MECH_AES_KEY_WRAP_PAD", IPA_PKCS11_MECH_AES_KEY_WRAP_PAD_obj);
	Py_XDECREF(IPA_PKCS11_MECH_AES_KEY_WRAP_PAD_obj);

	/* Key attributes */
	PyObject *IPA_PKCS11_ATTR_CKA_ALWAYS_AUTHENTICATE_obj = PyInt_FromLong(CKA_ALWAYS_AUTHENTICATE);
	PyObject_SetAttrString(m, "CKA_ALWAYS_AUTHENTICATE", IPA_PKCS11_ATTR_CKA_ALWAYS_AUTHENTICATE_obj);
	Py_XDECREF(IPA_PKCS11_ATTR_CKA_ALWAYS_AUTHENTICATE_obj);

	PyObject *IPA_PKCS11_ATTR_CKA_ALWAYS_SENSITIVE_obj = PyInt_FromLong(CKA_ALWAYS_SENSITIVE);
	PyObject_SetAttrString(m, "CKA_ALWAYS_SENSITIVE", IPA_PKCS11_ATTR_CKA_ALWAYS_SENSITIVE_obj);
	Py_XDECREF(IPA_PKCS11_ATTR_CKA_ALWAYS_SENSITIVE_obj);

	PyObject *IPA_PKCS11_ATTR_CKA_COPYABLE_obj = PyInt_FromLong(CKA_COPYABLE);
	PyObject_SetAttrString(m, "CKA_COPYABLE", IPA_PKCS11_ATTR_CKA_COPYABLE_obj);
	Py_XDECREF(IPA_PKCS11_ATTR_CKA_COPYABLE_obj);

	PyObject *IPA_PKCS11_ATTR_CKA_DECRYPT_obj = PyInt_FromLong(CKA_DECRYPT);
	PyObject_SetAttrString(m, "CKA_DECRYPT", IPA_PKCS11_ATTR_CKA_DECRYPT_obj);
	Py_XDECREF(IPA_PKCS11_ATTR_CKA_DECRYPT_obj);

	PyObject *IPA_PKCS11_ATTR_CKA_DERIVE_obj = PyInt_FromLong(CKA_DERIVE);
	PyObject_SetAttrString(m, "CKA_DERIVE", IPA_PKCS11_ATTR_CKA_DERIVE_obj);
	Py_XDECREF(IPA_PKCS11_ATTR_CKA_DERIVE_obj);

	PyObject *IPA_PKCS11_ATTR_CKA_ENCRYPT_obj = PyInt_FromLong(CKA_ENCRYPT);
	PyObject_SetAttrString(m, "CKA_ENCRYPT", IPA_PKCS11_ATTR_CKA_ENCRYPT_obj);
	Py_XDECREF(IPA_PKCS11_ATTR_CKA_ENCRYPT_obj);

	PyObject *IPA_PKCS11_ATTR_CKA_EXTRACTABLE_obj = PyInt_FromLong(CKA_EXTRACTABLE);
	PyObject_SetAttrString(m, "CKA_EXTRACTABLE", IPA_PKCS11_ATTR_CKA_EXTRACTABLE_obj);
	Py_XDECREF(IPA_PKCS11_ATTR_CKA_EXTRACTABLE_obj);

	PyObject *IPA_PKCS11_ATTR_CKA_ID_obj = PyInt_FromLong(CKA_ID);
	PyObject_SetAttrString(m, "CKA_ID", IPA_PKCS11_ATTR_CKA_ID_obj);
	Py_XDECREF(IPA_PKCS11_ATTR_CKA_ID_obj);

	PyObject *IPA_PKCS11_ATTR_CKA_KEY_TYPE_obj = PyInt_FromLong(CKA_KEY_TYPE);
	PyObject_SetAttrString(m, "CKA_KEY_TYPE", IPA_PKCS11_ATTR_CKA_KEY_TYPE_obj);
	Py_XDECREF(IPA_PKCS11_ATTR_CKA_KEY_TYPE_obj);

	PyObject *IPA_PKCS11_ATTR_CKA_LOCAL_obj = PyInt_FromLong(CKA_LOCAL);
	PyObject_SetAttrString(m, "CKA_LOCAL", IPA_PKCS11_ATTR_CKA_LOCAL_obj);
	Py_XDECREF(IPA_PKCS11_ATTR_CKA_LOCAL_obj);

	PyObject *IPA_PKCS11_ATTR_CKA_MODIFIABLE_obj = PyInt_FromLong(CKA_MODIFIABLE);
	PyObject_SetAttrString(m, "CKA_MODIFIABLE", IPA_PKCS11_ATTR_CKA_MODIFIABLE_obj);
	Py_XDECREF(IPA_PKCS11_ATTR_CKA_MODIFIABLE_obj);

	PyObject *IPA_PKCS11_ATTR_CKA_NEVER_EXTRACTABLE_obj = PyInt_FromLong(CKA_NEVER_EXTRACTABLE);
	PyObject_SetAttrString(m, "CKA_NEVER_EXTRACTABLE", IPA_PKCS11_ATTR_CKA_NEVER_EXTRACTABLE_obj);
	Py_XDECREF(IPA_PKCS11_ATTR_CKA_NEVER_EXTRACTABLE_obj);

	PyObject *IPA_PKCS11_ATTR_CKA_PRIVATE_obj = PyInt_FromLong(CKA_PRIVATE);
	PyObject_SetAttrString(m, "CKA_PRIVATE", IPA_PKCS11_ATTR_CKA_PRIVATE_obj);
	Py_XDECREF(IPA_PKCS11_ATTR_CKA_PRIVATE_obj);

	PyObject *IPA_PKCS11_ATTR_CKA_SENSITIVE_obj = PyInt_FromLong(CKA_SENSITIVE);
	PyObject_SetAttrString(m, "CKA_SENSITIVE", IPA_PKCS11_ATTR_CKA_SENSITIVE_obj);
	Py_XDECREF(IPA_PKCS11_ATTR_CKA_SENSITIVE_obj);

	PyObject *IPA_PKCS11_ATTR_CKA_SIGN_obj = PyInt_FromLong(CKA_SIGN);
	PyObject_SetAttrString(m, "CKA_SIGN", IPA_PKCS11_ATTR_CKA_SIGN_obj);
	Py_XDECREF(IPA_PKCS11_ATTR_CKA_SIGN_obj);

	PyObject *IPA_PKCS11_ATTR_CKA_SIGN_RECOVER_obj = PyInt_FromLong(CKA_SIGN_RECOVER);
	PyObject_SetAttrString(m, "CKA_SIGN_RECOVER", IPA_PKCS11_ATTR_CKA_SIGN_RECOVER_obj);
	Py_XDECREF(IPA_PKCS11_ATTR_CKA_SIGN_RECOVER_obj);

	PyObject *IPA_PKCS11_ATTR_CKA_TRUSTED_obj = PyInt_FromLong(CKA_TRUSTED);
	PyObject_SetAttrString(m, "CKA_TRUSTED", IPA_PKCS11_ATTR_CKA_TRUSTED_obj);
	Py_XDECREF(IPA_PKCS11_ATTR_CKA_TRUSTED_obj);

	PyObject *IPA_PKCS11_ATTR_CKA_VERIFY_obj = PyInt_FromLong(CKA_VERIFY);
	PyObject_SetAttrString(m, "CKA_VERIFY", IPA_PKCS11_ATTR_CKA_VERIFY_obj);
	Py_XDECREF(IPA_PKCS11_ATTR_CKA_VERIFY_obj);

	PyObject *IPA_PKCS11_ATTR_CKA_VERIFY_RECOVER_obj = PyInt_FromLong(CKA_VERIFY_RECOVER);
	PyObject_SetAttrString(m, "CKA_VERIFY_RECOVER", IPA_PKCS11_ATTR_CKA_VERIFY_RECOVER_obj);
	Py_XDECREF(IPA_PKCS11_ATTR_CKA_VERIFY_RECOVER_obj);

	PyObject *IPA_PKCS11_ATTR_CKA_UNWRAP_obj = PyInt_FromLong(CKA_UNWRAP);
	PyObject_SetAttrString(m, "CKA_UNWRAP", IPA_PKCS11_ATTR_CKA_UNWRAP_obj);
	Py_XDECREF(IPA_PKCS11_ATTR_CKA_UNWRAP_obj);

	PyObject *IPA_PKCS11_ATTR_CKA_WRAP_obj = PyInt_FromLong(CKA_WRAP);
	PyObject_SetAttrString(m, "CKA_WRAP", IPA_PKCS11_ATTR_CKA_WRAP_obj);
	Py_XDECREF(IPA_PKCS11_ATTR_CKA_WRAP_obj);

	PyObject *IPA_PKCS11_ATTR_CKA_WRAP_WITH_TRUSTED_obj = PyInt_FromLong(CKA_WRAP_WITH_TRUSTED);
	PyObject_SetAttrString(m, "CKA_WRAP_WITH_TRUSTED", IPA_PKCS11_ATTR_CKA_WRAP_WITH_TRUSTED_obj);
	Py_XDECREF(IPA_PKCS11_ATTR_CKA_WRAP_WITH_TRUSTED_obj);

	PyObject *IPA_PKCS11_ATTR_CKA_TOKEN_obj = PyInt_FromLong(CKA_TOKEN);
	PyObject_SetAttrString(m, "CKA_TOKEN", IPA_PKCS11_ATTR_CKA_TOKEN_obj);
	Py_XDECREF(IPA_PKCS11_ATTR_CKA_TOKEN_obj);

	PyObject *IPA_PKCS11_ATTR_CKA_LABEL_obj = PyInt_FromLong(CKA_LABEL);
	PyObject_SetAttrString(m, "CKA_LABEL", IPA_PKCS11_ATTR_CKA_LABEL_obj);
	Py_XDECREF(IPA_PKCS11_ATTR_CKA_LABEL_obj);

}
