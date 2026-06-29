package com.example.gatewaycommunicatorgen2;

import javax.net.ssl.*;
import java.security.MessageDigest;
import java.security.cert.*;
import java.util.Base64;

public class CertPin {

    // Wstaw tutaj fingerprint swojego certyfikatu (bez dwukropków, wielkimi literami)
    //Klucz do RSA
    private static final String PINNED_SHA256 = "2C8DB42E24E2C5396F20898243C1A4EB3E0A4B3740B7ADBC1CD2B1344DF22B34";
    //Klucz do ECDSA
    //private static final String PINNED_SHA256 = "FAC934CDE277D2DF8B66EBDF063FAEDB6C4AA407BB94ADB461F70928EA2A7F7B";

    // The pinning trust manager (leaf SHA-256). Exposed so OkHttp (WebSocket) can
    // use sslSocketFactory(factory, trustManager) - same pin as HttpsURLConnection.
    public static X509TrustManager getPinnedTrustManager() {
        return new X509TrustManager() {
            public void checkClientTrusted(X509Certificate[] chain, String authType) {}
            public void checkServerTrusted(X509Certificate[] chain, String authType) throws CertificateException {
                try {
                    MessageDigest md = MessageDigest.getInstance("SHA-256");
                    byte[] hash = md.digest(chain[0].getEncoded());
                    String actual = bytesToHex(hash);
                    if (!actual.equalsIgnoreCase(PINNED_SHA256)) {
                        throw new CertificateException("Certyfikat niezgodny z pinned SHA-256!");
                    }
                } catch (CertificateException e) {
                    throw e;
                } catch (Exception e) {
                    throw new CertificateException("Błąd przetwarzania certyfikatu: " + e.getMessage());
                }
            }
            public X509Certificate[] getAcceptedIssuers() { return new X509Certificate[0]; }
        };
    }

    public static SSLSocketFactory getPinnedFactory() throws Exception {
        SSLContext context = SSLContext.getInstance("TLS");
        context.init(null, new TrustManager[]{ getPinnedTrustManager() }, null);
        return context.getSocketFactory();
    }

    private static String bytesToHex(byte[] bytes) {
        StringBuilder sb = new StringBuilder();
        for (byte b : bytes)
            sb.append(String.format("%02X", b));
        return sb.toString();
    }
}