/*******************************************************************************
*   Ledger App - Bitcoin Wallet
*   (c) 2016-2019 Ledger
*
*  Licensed under the Apache License, Version 2.0 (the "License");
*  you may not use this file except in compliance with the License.
*  You may obtain a copy of the License at
*
*      http://www.apache.org/licenses/LICENSE-2.0
*
*  Unless required by applicable law or agreed to in writing, software
*  distributed under the License is distributed on an "AS IS" BASIS,
*  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
*  See the License for the specific language governing permissions and
*  limitations under the License.
********************************************************************************/

#include "btchip_internal.h"
#include "btchip_apdu_constants.h"

#define SIGHASH_ALL 0x01

unsigned short btchip_apdu_hash_sign() {
    unsigned long int lockTime;
    uint32_t sighashType;
    unsigned char dataBuffer[8];
    unsigned char hash1[32];
    unsigned char hash2[32];
    //TODO    unsigned char hash3[32];
    unsigned char authorizationLength;
    unsigned char *parameters = G_io_apdu_buffer + ISO_OFFSET_CDATA;
    btchip_transaction_summary_t
        transactionSummary; // could be removed with a refactor
    unsigned short sw;
    unsigned char keyPath[MAX_BIP32_PATH_LENGTH];
    cx_sha256_t localHash;

    SB_CHECK(N_btchip.bkp.config.operationMode);
    switch (SB_GET(N_btchip.bkp.config.operationMode)) {
    case BTCHIP_MODE_WALLET:
    case BTCHIP_MODE_RELAXED_WALLET:
    case BTCHIP_MODE_SERVER:
        break;
    default:
        return BTCHIP_SW_CONDITIONS_OF_USE_NOT_SATISFIED;
    }

    if ((G_io_apdu_buffer[ISO_OFFSET_P1] != 0) &&
        (G_io_apdu_buffer[ISO_OFFSET_P2] != 0)) {
        return BTCHIP_SW_INCORRECT_P1_P2;
    }

    if (G_io_apdu_buffer[ISO_OFFSET_LC] < (1 + 1 + 4 + 1)) {
        return BTCHIP_SW_INCORRECT_LENGTH;
    }
    PRINTF("buffer 1\n%.*H\n", sizeof(G_io_apdu_buffer), G_io_apdu_buffer);
    // Check state
    BEGIN_TRY {
        TRY {
            btchip_set_check_internal_structure_integrity(0);

            // Zcash special - store parameters for later

            if ((btchip_context_D.usingOverwinter) &&
                (!btchip_context_D.overwinterSignReady) &&
                (btchip_context_D.segwitParsedOnce) &&
                (btchip_context_D.transactionContext.transactionState == BTCHIP_TRANSACTION_NONE)) {
                unsigned long int expiryHeight;
                parameters += (4 * G_io_apdu_buffer[ISO_OFFSET_CDATA]) + 1;
                authorizationLength = *(parameters++);
                parameters += authorizationLength;
                lockTime = btchip_read_u32(parameters, 1, 0);
                parameters += 4;
                sighashType = *(parameters++);
                expiryHeight = btchip_read_u32(parameters, 1, 0);
                btchip_write_u32_le(btchip_context_D.nLockTime, lockTime);
                btchip_write_u32_le(btchip_context_D.sigHashType, sighashType);
                btchip_write_u32_le(btchip_context_D.nExpiryHeight, expiryHeight);
                btchip_context_D.overwinterSignReady = 1;
                CLOSE_TRY;
                return BTCHIP_SW_OK;
            }

	    if (btchip_context_D.transactionContext.transactionState !=
		BTCHIP_TRANSACTION_SIGN_READY) {
	      PRINTF("Invalid transaction state %d\n", btchip_context_D.transactionContext.transactionState);
	      sw = BTCHIP_SW_CONDITIONS_OF_USE_NOT_SATISFIED;
	      goto discardTransaction;
	    }

            if (btchip_context_D.usingOverwinter && !btchip_context_D.overwinterSignReady) {
                PRINTF("Overwinter not ready to sign\n");
                sw = BTCHIP_SW_CONDITIONS_OF_USE_NOT_SATISFIED;
                goto discardTransaction;
            }

	    
	    PRINTF("Hash sign: read parameters\n");
            // Read parameters
            if (G_io_apdu_buffer[ISO_OFFSET_CDATA] > MAX_BIP32_PATH) {
                sw = BTCHIP_SW_INCORRECT_DATA;
            discardTransaction:
                CLOSE_TRY;
                goto catch_discardTransaction;
            }
	    PRINTF("buffer 2\n%.*H\n", sizeof(G_io_apdu_buffer), G_io_apdu_buffer);
            os_memmove(keyPath, G_io_apdu_buffer + ISO_OFFSET_CDATA,
                       MAX_BIP32_PATH_LENGTH);
            parameters += (4 * G_io_apdu_buffer[ISO_OFFSET_CDATA]) + 1;
            authorizationLength = *(parameters++);
            parameters += authorizationLength;
            lockTime = btchip_read_u32(parameters, 1, 0);
            parameters += 4;
            sighashType = *(parameters++);

            if (((N_btchip.bkp.config.options &
                  BTCHIP_OPTION_FREE_SIGHASHTYPE) == 0)) {
                // if bitcoin cash OR forkid is set, then use the fork id
                if (G_coin_config->kind == COIN_KIND_BITCOIN_CASH ||
                    (G_coin_config->forkid)) {
#define SIGHASH_FORKID 0x40
                    if (sighashType != (SIGHASH_ALL | SIGHASH_FORKID)) {
                        sw = BTCHIP_SW_INCORRECT_DATA;
                        goto discardTransaction;
                    }
                    sighashType |= (G_coin_config->forkid << 8);
                } else {
                    if (sighashType != SIGHASH_ALL) {
                        sw = BTCHIP_SW_INCORRECT_DATA;
                        goto discardTransaction;
                    }
                }
            }
	    PRINTF("buffer 3\n%.*H\n", sizeof(G_io_apdu_buffer), G_io_apdu_buffer);
	    PRINTF("Hash sign: finished read transaction parameters\n");
            // Read transaction parameters
            // TODO : remove copy
            os_memmove(&transactionSummary,
                       &btchip_context_D.transactionSummary,
                       sizeof(transactionSummary));

	    
            // Fetch the private key

	    if (!os_global_pin_is_validated()) {
	      return BTCHIP_SW_SECURITY_STATUS_NOT_SATISFIED;
	    }

	    PRINTF("Hash sign: fetch the private key - keyPath\n%.*H\n", sizeof(keyPath), keyPath);
            btchip_private_derive_keypair(keyPath, 0, NULL);

            // TODO optional : check the public key against the associated non
            // blank input to sign

	    PRINTF("Hash sign: finalize the hash\n");
            // Finalize the hash

            if (btchip_context_D.usingOverwinter) {
                cx_hash(&btchip_context_D.transactionHashFull.blake2b.header, CX_LAST, hash2, 0, hash2, 32);
            }
            else {
                btchip_write_u32_le(dataBuffer, lockTime);
                btchip_write_u32_le(dataBuffer + 4, sighashType);
                PRINTF("Finalize hash with\n%.*H\n", sizeof(dataBuffer), dataBuffer);

		if (!os_global_pin_is_validated()) {
		  return BTCHIP_SW_SECURITY_STATUS_NOT_SATISFIED;
		}
		
                cx_hash(&btchip_context_D.transactionHashFull.sha256.header, CX_LAST,
                    dataBuffer, sizeof(dataBuffer), hash1, 32);
		PRINTF("Hash1\n%.*H\n", sizeof(hash1), hash1);

		if (!os_global_pin_is_validated()) {
		  return BTCHIP_SW_SECURITY_STATUS_NOT_SATISFIED;
		}
		
                // Rehash
                cx_sha256_init(&localHash);

		if (!os_global_pin_is_validated()) {
		  return BTCHIP_SW_SECURITY_STATUS_NOT_SATISFIED;
		}
		
                cx_hash(&localHash.header, CX_LAST, hash1, sizeof(hash1), hash2, 32);
            }
            PRINTF("Hash2\n%.*H\n", sizeof(hash2), hash2);

	    PRINTF("Hash sign: sign\n");
	    if (!os_global_pin_is_validated()) {
	      return BTCHIP_SW_SECURITY_STATUS_NOT_SATISFIED;
	    }

	    PRINTF("Outlength 0\n%d\n", btchip_context_D.outLength);

	    PRINTF("Private key:\n");
	    PRINTF("%.*H\n", sizeof(btchip_private_key_D), btchip_private_key_D);
	    
            // Sign
	    PRINTF("buffer 4\n%.*H\n", sizeof(G_io_apdu_buffer), G_io_apdu_buffer);
	    btchip_signverify_finalhash(
					&btchip_private_key_D, 1, hash2, sizeof(hash2),
					G_io_apdu_buffer, sizeof(G_io_apdu_buffer),
					((N_btchip.bkp.config.options &
					  BTCHIP_OPTION_DETERMINISTIC_SIGNATURE) != 0));

	    //	    btchip_signverify_finalhash(
	    //	                    &btchip_private_key_D, 0, 
			      //	                    G_io_apdu_buffer, sizeof(G_io_apdu_buffer),
			      //			    hash3, sizeof(hash3),
			      ///	                    ((N_btchip.bkp.config.options &
			      //	                      BTCHIP_OPTION_DETERMINISTIC_SIGNATURE) != 0));
		
	    PRINTF("buffer 5\n%.*H\n", sizeof(G_io_apdu_buffer), G_io_apdu_buffer);
	    PRINTF("buffer length\n%d\n", sizeof(G_io_apdu_buffer));
	    PRINTF("buffer[1]\n%d\n",G_io_apdu_buffer[1]);
	    PRINTF("outlength 1\n%d\n", G_io_apdu_buffer[1]+2);
	    PRINTF("Setting outlength");
	    btchip_context_D.outLength = G_io_apdu_buffer[1] + 2;
	    PRINTF("Finished seting outlength");
	    PRINTF("Outlength\n%d\n", btchip_context_D.outLength);
	    PRINTF("sighashType\n%d\n", sighashType);
	    G_io_apdu_buffer[btchip_context_D.outLength++] = sighashType;
	    PRINTF("buffer 6\n%.*H\n", btchip_context_D.outLength, G_io_apdu_buffer);

	    PRINTF("Hash sign: finished - io exchange\n");
	    //TODO
	    //io_exchange(CHANNEL_APDU | IO_RETURN_AFTER_TX, btchip_context_D.outLength);
            sw = BTCHIP_SW_OK;

            // Then discard the transaction and reply
        }
        CATCH_ALL {
  	  PRINTF("Hash sign: exception caught\n");
            sw = SW_TECHNICAL_DETAILS(0xF);
        catch_discardTransaction:
            btchip_context_D.transactionContext.transactionState =
                BTCHIP_TRANSACTION_NONE;
        }
        FINALLY {
	  btchip_set_check_internal_structure_integrity(1);
	  PRINTF("Hash sign: returning`\n");
	  return sw;
        }
    }
    END_TRY;
}
