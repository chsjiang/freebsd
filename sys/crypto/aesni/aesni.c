/*-
 * Copyright (c) 2005-2008 Pawel Jakub Dawidek <pjd@FreeBSD.org>
 * Copyright (c) 2010 Konstantin Belousov <kib@FreeBSD.org>
 * Copyright (c) 2014 The FreeBSD Foundation
 * All rights reserved.
 *
 * Portions of this software were developed by John-Mark Gurney
 * under sponsorship of the FreeBSD Foundation and
 * Rubicon Communications, LLC (Netgate).
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHORS AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHORS OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

#include <sys/cdefs.h>
__FBSDID("$FreeBSD$");

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/kernel.h>
#include <sys/kobj.h>
#include <sys/libkern.h>
#include <sys/lock.h>
#include <sys/module.h>
#include <sys/malloc.h>
#include <sys/rwlock.h>
#include <sys/bus.h>
#include <sys/uio.h>
#include <sys/mbuf.h>
#include <crypto/aesni/aesni.h>
#include <cryptodev_if.h>
#include <opencrypto/gmac.h>

struct aesni_softc {
	int32_t cid;
	uint32_t sid;
	TAILQ_HEAD(aesni_sessions_head, aesni_session) sessions;
	struct rwlock lock;
};

static int aesni_newsession(device_t, uint32_t *sidp, struct cryptoini *cri);
static int aesni_freesession(device_t, uint64_t tid);
static void aesni_freesession_locked(struct aesni_softc *sc,
    struct aesni_session *ses);
static int aesni_cipher_setup(struct aesni_session *ses,
    struct cryptoini *encini);
static int aesni_cipher_process(struct aesni_session *ses,
    struct cryptodesc *enccrd, struct cryptodesc *authcrd, struct cryptop *crp);

MALLOC_DEFINE(M_AESNI, "aesni_data", "AESNI Data");

static void
aesni_identify(driver_t *drv, device_t parent)
{

	/* NB: order 10 is so we get attached after h/w devices */
	if (device_find_child(parent, "aesni", -1) == NULL &&
	    BUS_ADD_CHILD(parent, 10, "aesni", -1) == 0)
		panic("aesni: could not attach");
}

static int
aesni_probe(device_t dev)
{

	if ((cpu_feature2 & CPUID2_AESNI) == 0) {
		device_printf(dev, "No AESNI support.\n");
		return (EINVAL);
	}

	if ((cpu_feature2 & CPUID2_SSE41) == 0) {
		device_printf(dev, "No SSE4.1 support.\n");
		return (EINVAL);
	}

	device_set_desc_copy(dev, "AES-CBC,AES-XTS,AES-GCM,AES-ICM");
	return (0);
}

static int
aesni_attach(device_t dev)
{
	struct aesni_softc *sc;

	sc = device_get_softc(dev);
	TAILQ_INIT(&sc->sessions);
	sc->sid = 1;
	sc->cid = crypto_get_driverid(dev, CRYPTOCAP_F_HARDWARE |
	    CRYPTOCAP_F_SYNC);
	if (sc->cid < 0) {
		device_printf(dev, "Could not get crypto driver id.\n");
		return (ENOMEM);
	}

	rw_init(&sc->lock, "aesni_lock");
	crypto_register(sc->cid, CRYPTO_AES_CBC, 0, 0);
	crypto_register(sc->cid, CRYPTO_AES_ICM, 0, 0);
	crypto_register(sc->cid, CRYPTO_AES_NIST_GCM_16, 0, 0);
	crypto_register(sc->cid, CRYPTO_AES_128_NIST_GMAC, 0, 0);
	crypto_register(sc->cid, CRYPTO_AES_192_NIST_GMAC, 0, 0);
	crypto_register(sc->cid, CRYPTO_AES_256_NIST_GMAC, 0, 0);
	crypto_register(sc->cid, CRYPTO_AES_XTS, 0, 0);
	return (0);
}

static int
aesni_detach(device_t dev)
{
	struct aesni_softc *sc;
	struct aesni_session *ses;

	sc = device_get_softc(dev);
	rw_wlock(&sc->lock);
	TAILQ_FOREACH(ses, &sc->sessions, next) {
		if (ses->used) {
			rw_wunlock(&sc->lock);
			device_printf(dev,
			    "Cannot detach, sessions still active.\n");
			return (EBUSY);
		}
	}
	while ((ses = TAILQ_FIRST(&sc->sessions)) != NULL) {
		TAILQ_REMOVE(&sc->sessions, ses, next);
		fpu_kern_free_ctx(ses->fpu_ctx);
		free(ses, M_AESNI);
	}
	rw_wunlock(&sc->lock);
	rw_destroy(&sc->lock);
	crypto_unregister_all(sc->cid);
	return (0);
}

static int
aesni_newsession(device_t dev, uint32_t *sidp, struct cryptoini *cri)
{
	struct aesni_softc *sc;
	struct aesni_session *ses;
	struct cryptoini *encini;
	int error;

	if (sidp == NULL || cri == NULL) {
		CRYPTDEB("no sidp or cri");
		return (EINVAL);
	}

	sc = device_get_softc(dev);
	ses = NULL;
	encini = NULL;
	for (; cri != NULL; cri = cri->cri_next) {
		switch (cri->cri_alg) {
		case CRYPTO_AES_CBC:
		case CRYPTO_AES_ICM:
		case CRYPTO_AES_XTS:
		case CRYPTO_AES_NIST_GCM_16:
			if (encini != NULL) {
				CRYPTDEB("encini already set");
				return (EINVAL);
			}
			encini = cri;
			break;
		case CRYPTO_AES_128_NIST_GMAC:
		case CRYPTO_AES_192_NIST_GMAC:
		case CRYPTO_AES_256_NIST_GMAC:
			/*
			 * nothing to do here, maybe in the future cache some
			 * values for GHASH
			 */
			break;
		default:
			CRYPTDEB("unhandled algorithm");
			return (EINVAL);
		}
	}
	if (encini == NULL) {
		CRYPTDEB("no cipher");
		return (EINVAL);
	}

	rw_wlock(&sc->lock);
	/*
	 * Free sessions goes first, so if first session is used, we need to
	 * allocate one.
	 */
	ses = TAILQ_FIRST(&sc->sessions);
	if (ses == NULL || ses->used) {
		ses = malloc(sizeof(*ses), M_AESNI, M_NOWAIT | M_ZERO);
		if (ses == NULL) {
			rw_wunlock(&sc->lock);
			return (ENOMEM);
		}
		ses->fpu_ctx = fpu_kern_alloc_ctx(FPU_KERN_NORMAL |
		    FPU_KERN_NOWAIT);
		if (ses->fpu_ctx == NULL) {
			free(ses, M_AESNI);
			rw_wunlock(&sc->lock);
			return (ENOMEM);
		}
		ses->id = sc->sid++;
	} else {
		TAILQ_REMOVE(&sc->sessions, ses, next);
	}
	ses->used = 1;
	TAILQ_INSERT_TAIL(&sc->sessions, ses, next);
	rw_wunlock(&sc->lock);
	ses->algo = encini->cri_alg;

	error = aesni_cipher_setup(ses, encini);
	if (error != 0) {
		CRYPTDEB("setup failed");
		rw_wlock(&sc->lock);
		aesni_freesession_locked(sc, ses);
		rw_wunlock(&sc->lock);
		return (error);
	}

	*sidp = ses->id;
	return (0);
}

static void
aesni_freesession_locked(struct aesni_softc *sc, struct aesni_session *ses)
{
	struct fpu_kern_ctx *ctx;
	uint32_t sid;

	sid = ses->id;
	TAILQ_REMOVE(&sc->sessions, ses, next);
	ctx = ses->fpu_ctx;
	*ses = (struct aesni_session){};
	ses->id = sid;
	ses->fpu_ctx = ctx;
	TAILQ_INSERT_HEAD(&sc->sessions, ses, next);
}

static int
aesni_freesession(device_t dev, uint64_t tid)
{
	struct aesni_softc *sc;
	struct aesni_session *ses;
	uint32_t sid;

	sc = device_get_softc(dev);
	sid = ((uint32_t)tid) & 0xffffffff;
	rw_wlock(&sc->lock);
	TAILQ_FOREACH_REVERSE(ses, &sc->sessions, aesni_sessions_head, next) {
		if (ses->id == sid)
			break;
	}
	if (ses == NULL) {
		rw_wunlock(&sc->lock);
		return (EINVAL);
	}
	aesni_freesession_locked(sc, ses);
	rw_wunlock(&sc->lock);
	return (0);
}

static int
aesni_process(device_t dev, struct cryptop *crp, int hint __unused)
{
	struct aesni_softc *sc = device_get_softc(dev);
	struct aesni_session *ses = NULL;
	struct cryptodesc *crd, *enccrd, *authcrd;
	int error, needauth;

	error = 0;
	enccrd = NULL;
	authcrd = NULL;
	needauth = 0;

	/* Sanity check. */
	if (crp == NULL)
		return (EINVAL);

	if (crp->crp_callback == NULL || crp->crp_desc == NULL) {
		error = EINVAL;
		goto out;
	}

	for (crd = crp->crp_desc; crd != NULL; crd = crd->crd_next) {
		switch (crd->crd_alg) {
		case CRYPTO_AES_CBC:
		case CRYPTO_AES_ICM:
		case CRYPTO_AES_XTS:
			if (enccrd != NULL) {
				error = EINVAL;
				goto out;
			}
			enccrd = crd;
			break;

		case CRYPTO_AES_NIST_GCM_16:
			if (enccrd != NULL) {
				error = EINVAL;
				goto out;
			}
			enccrd = crd;
			needauth = 1;
			break;

		case CRYPTO_AES_128_NIST_GMAC:
		case CRYPTO_AES_192_NIST_GMAC:
		case CRYPTO_AES_256_NIST_GMAC:
			if (authcrd != NULL) {
				error = EINVAL;
				goto out;
			}
			authcrd = crd;
			needauth = 1;
			break;

		default:
			error = EINVAL;
			goto out;
		}
	}

	if (enccrd == NULL || (needauth && authcrd == NULL)) {
		error = EINVAL;
		goto out;
	}

	/* CBC & XTS can only handle full blocks for now */
	if ((enccrd->crd_alg == CRYPTO_AES_CBC || enccrd->crd_alg ==
	    CRYPTO_AES_XTS) && (enccrd->crd_len % AES_BLOCK_LEN) != 0) {
		error = EINVAL;
		goto out;
	}

	rw_rlock(&sc->lock);
	TAILQ_FOREACH_REVERSE(ses, &sc->sessions, aesni_sessions_head, next) {
		if (ses->id == (crp->crp_sid & 0xffffffff))
			break;
	}
	rw_runlock(&sc->lock);
	if (ses == NULL) {
		error = EINVAL;
		goto out;
	}

	error = aesni_cipher_process(ses, enccrd, authcrd, crp);
	if (error != 0)
		goto out;

out:
	crp->crp_etype = error;
	crypto_done(crp);
	return (error);
}

uint8_t *
aesni_cipher_alloc(struct cryptodesc *enccrd, struct cryptop *crp,
    int *allocated)
{
	struct mbuf *m;
	struct uio *uio;
	struct iovec *iov;
	uint8_t *addr;

	if (crp->crp_flags & CRYPTO_F_IMBUF) {
		m = (struct mbuf *)crp->crp_buf;
		if (m->m_next != NULL)
			goto alloc;
		addr = mtod(m, uint8_t *);
	} else if (crp->crp_flags & CRYPTO_F_IOV) {
		uio = (struct uio *)crp->crp_buf;
		if (uio->uio_iovcnt != 1)
			goto alloc;
		iov = uio->uio_iov;
		addr = (uint8_t *)iov->iov_base;
	} else
		addr = (uint8_t *)crp->crp_buf;
	*allocated = 0;
	addr += enccrd->crd_skip;
	return (addr);

alloc:
	addr = malloc(enccrd->crd_len, M_AESNI, M_NOWAIT);
	if (addr != NULL) {
		*allocated = 1;
		crypto_copydata(crp->crp_flags, crp->crp_buf, enccrd->crd_skip,
		    enccrd->crd_len, addr);
	} else
		*allocated = 0;
	return (addr);
}

static device_method_t aesni_methods[] = {
	DEVMETHOD(device_identify, aesni_identify),
	DEVMETHOD(device_probe, aesni_probe),
	DEVMETHOD(device_attach, aesni_attach),
	DEVMETHOD(device_detach, aesni_detach),

	DEVMETHOD(cryptodev_newsession, aesni_newsession),
	DEVMETHOD(cryptodev_freesession, aesni_freesession),
	DEVMETHOD(cryptodev_process, aesni_process),

	{0, 0},
};

static driver_t aesni_driver = {
	"aesni",
	aesni_methods,
	sizeof(struct aesni_softc),
};
static devclass_t aesni_devclass;

DRIVER_MODULE(aesni, nexus, aesni_driver, aesni_devclass, 0, 0);
MODULE_VERSION(aesni, 1);
MODULE_DEPEND(aesni, crypto, 1, 1, 1);

static int
aesni_cipher_setup(struct aesni_session *ses, struct cryptoini *encini)
{
	struct thread *td;
	int error;

	td = curthread;
	error = fpu_kern_enter(td, ses->fpu_ctx, FPU_KERN_NORMAL |
	    FPU_KERN_KTHR);
	if (error != 0)
		return (error);
	error = aesni_cipher_setup_common(ses, encini->cri_key,
	    encini->cri_klen);
	fpu_kern_leave(td, ses->fpu_ctx);
	return (error);
}

/*
 * authcrd contains the associated date.
 */
static int
aesni_cipher_process(struct aesni_session *ses, struct cryptodesc *enccrd,
    struct cryptodesc *authcrd, struct cryptop *crp)
{
	uint8_t tag[GMAC_DIGEST_LEN];
	struct thread *td;
	uint8_t *buf, *authbuf;
	int error, allocated, authallocated;
	int ivlen, encflag;

	encflag = (enccrd->crd_flags & CRD_F_ENCRYPT) == CRD_F_ENCRYPT;

	if ((enccrd->crd_alg == CRYPTO_AES_ICM ||
	    enccrd->crd_alg == CRYPTO_AES_NIST_GCM_16) &&
	    (enccrd->crd_flags & CRD_F_IV_EXPLICIT) == 0)
		return (EINVAL);

	buf = aesni_cipher_alloc(enccrd, crp, &allocated);
	if (buf == NULL)
		return (ENOMEM);

	authbuf = NULL;
	authallocated = 0;
	if (authcrd != NULL) {
		authbuf = aesni_cipher_alloc(authcrd, crp, &authallocated);
		if (authbuf == NULL) {
			error = ENOMEM;
			goto out1;
		}
	}

	td = curthread;
	error = fpu_kern_enter(td, ses->fpu_ctx, FPU_KERN_NORMAL |
	    FPU_KERN_KTHR);
	if (error != 0)
		goto out1;

	if ((enccrd->crd_flags & CRD_F_KEY_EXPLICIT) != 0) {
		error = aesni_cipher_setup_common(ses, enccrd->crd_key,
		    enccrd->crd_klen);
		if (error != 0)
			goto out;
	}

	/* XXX - validate that enccrd and authcrd have/use same key? */
	switch (enccrd->crd_alg) {
	case CRYPTO_AES_CBC:
	case CRYPTO_AES_ICM:
		ivlen = AES_BLOCK_LEN;
		break;
	case CRYPTO_AES_XTS:
		ivlen = 8;
		break;
	case CRYPTO_AES_NIST_GCM_16:
		ivlen = 12;	/* should support arbitarily larger */
		break;
	}

	/* Setup ses->iv */
	bzero(ses->iv, sizeof ses->iv);
	if ((enccrd->crd_flags & CRD_F_IV_EXPLICIT) != 0)
		bcopy(enccrd->crd_iv, ses->iv, ivlen);
	else if (encflag && ((enccrd->crd_flags & CRD_F_IV_PRESENT) != 0))
		arc4rand(ses->iv, ivlen, 0);
	else
		crypto_copydata(crp->crp_flags, crp->crp_buf,
		    enccrd->crd_inject, ivlen, ses->iv);

	if (authcrd != NULL && !encflag)
		crypto_copydata(crp->crp_flags, crp->crp_buf,
		    authcrd->crd_inject, GMAC_DIGEST_LEN, tag);
	else
		bzero(tag, sizeof tag);

	/* Do work */
	switch (ses->algo) {
	case CRYPTO_AES_CBC:
		if (encflag)
			aesni_encrypt_cbc(ses->rounds, ses->enc_schedule,
			    enccrd->crd_len, buf, buf, ses->iv);
		else
			aesni_decrypt_cbc(ses->rounds, ses->dec_schedule,
			    enccrd->crd_len, buf, ses->iv);
		break;
	case CRYPTO_AES_ICM:
		/* encryption & decryption are the same */
		aesni_encrypt_icm(ses->rounds, ses->enc_schedule,
		    enccrd->crd_len, buf, buf, ses->iv);
		break;
	case CRYPTO_AES_XTS:
		if (encflag)
			aesni_encrypt_xts(ses->rounds, ses->enc_schedule,
			    ses->xts_schedule, enccrd->crd_len, buf, buf,
			    ses->iv);
		else
			aesni_decrypt_xts(ses->rounds, ses->dec_schedule,
			    ses->xts_schedule, enccrd->crd_len, buf, buf,
			    ses->iv);
		break;
	case CRYPTO_AES_NIST_GCM_16:
		if (encflag)
			AES_GCM_encrypt(buf, buf, authbuf, ses->iv, tag,
			    enccrd->crd_len, authcrd->crd_len, ivlen,
			    ses->enc_schedule, ses->rounds);
		else {
			if (!AES_GCM_decrypt(buf, buf, authbuf, ses->iv, tag,
			    enccrd->crd_len, authcrd->crd_len, ivlen,
			    ses->enc_schedule, ses->rounds))
				error = EBADMSG;
		}
		break;
	}

	if (allocated)
		crypto_copyback(crp->crp_flags, crp->crp_buf, enccrd->crd_skip,
		    enccrd->crd_len, buf);

	/*
	 * OpenBSD doesn't copy this back.  This primes the IV for the next
	 * chain.  Why do we not do it for decrypt?
	 */
	if (encflag && enccrd->crd_alg == CRYPTO_AES_CBC)
		bcopy(buf + enccrd->crd_len - AES_BLOCK_LEN, ses->iv, AES_BLOCK_LEN);

	if (!error && authcrd != NULL) {
		crypto_copyback(crp->crp_flags, crp->crp_buf,
		    authcrd->crd_inject, GMAC_DIGEST_LEN, tag);
	}

out:
	fpu_kern_leave(td, ses->fpu_ctx);
out1:
	if (allocated) {
		bzero(buf, enccrd->crd_len);
		free(buf, M_AESNI);
	}
	if (authallocated)
		free(authbuf, M_AESNI);
	return (error);
}
