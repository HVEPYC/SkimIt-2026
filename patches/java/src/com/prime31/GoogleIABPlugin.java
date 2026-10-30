package com.prime31;

/**
 * Offline stand-in for Prime31's Google In-App Billing plugin.
 *
 * The billing service no longer exists for this game, so every request ends the way the
 * original did when the service was unavailable: init reports billingNotSupported with the
 * original helper's message, and queries/purchases/consumes report failure.
 */
public class GoogleIABPlugin {
    private static final String MANAGER = "GoogleIABManager";
    private static GoogleIABPlugin instance;

    public static synchronized GoogleIABPlugin instance() {
        if (instance == null) instance = new GoogleIABPlugin();
        return instance;
    }

    public void enableLogging(boolean shouldEnable) {}

    public void setAutoVerifySignatures(boolean shouldVerify) {}

    public void init(String publicKey) {
        Bridge.sendLater(0, MANAGER, "billingNotSupported", Bridge.BILLING_UNAVAILABLE);
    }

    public void unbindService() {}

    public boolean areSubscriptionsSupported() {
        return false;
    }

    public void queryInventory(String[] skus) {
        Bridge.sendLater(0, MANAGER, "queryInventoryFailed", Bridge.BILLING_UNAVAILABLE);
    }

    public void purchaseProduct(String sku, String developerPayload) {
        Bridge.sendLater(0, MANAGER, "purchaseFailed", Bridge.BILLING_UNAVAILABLE);
    }

    public void consumeProduct(String sku) {
        Bridge.sendLater(0, MANAGER, "consumePurchaseFailed", Bridge.BILLING_UNAVAILABLE);
    }

    public void consumeProducts(String[] skus) {
        Bridge.sendLater(0, MANAGER, "consumePurchaseFailed", Bridge.BILLING_UNAVAILABLE);
    }

    // Billing-helper callbacks of the original plugin, kept so the method set is identical.
    // Without a billing service they are never invoked.
    public void onConsumeMultiFinished(java.util.List purchases, java.util.List results) {}

    public void onIabPurchaseCompleteAwaitingVerification(String purchaseData, String signature) {}
}
