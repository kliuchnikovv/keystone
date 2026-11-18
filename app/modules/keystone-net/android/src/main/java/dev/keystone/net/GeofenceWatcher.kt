package dev.keystone.net

import android.Manifest
import android.annotation.SuppressLint
import android.app.PendingIntent
import android.content.Context
import android.content.Intent
import android.content.pm.PackageManager
import android.os.Build
import androidx.core.content.ContextCompat
import com.google.android.gms.location.Geofence
import com.google.android.gms.location.GeofencingClient
import com.google.android.gms.location.GeofencingRequest
import com.google.android.gms.location.LocationServices
import com.google.android.gms.location.LocationRequest
import com.google.android.gms.location.LocationCallback
import com.google.android.gms.location.LocationResult
import com.google.android.gms.location.LocationServices
import com.google.android.gms.tasks.Task
import kotlin.math.max
import kotlin.math.min

/**
 * Watches a circular region and says when the phone enters or leaves it.
 *
 * The geofence is what makes "when I come home" work: the app is killed by
 * Android as soon as it goes to the background, but the system's geofence
 * service still wakes us when the phone crosses the zone.
 *
 * Two details matter for a home automation:
 *
 *  - **Hysteresis.** A single radius makes the light flicker on the edge of the
 *    zone: entering at 100 m and leaving at 100 m means a short walk past the
 *    gate toggles it. So there are two radii — enter at the inner one, leave at
 *    the outer one.
 *  - **Confirmation.** Leaving for five minutes to buy bread should not turn
 *    the lights off. So "away" is only reported after the phone has been
 *    outside for a while; the confirmation is done here, in the native code, so
 *    the hub only ever sees clean "home" / "away".
 */
class GeofenceWatcher(
    private val context: Context,
    private val onChange: (Boolean) -> Unit,
    private val onError: (String) -> Unit,
) {
    companion object {
        /** How far outside the zone the phone must get before "away" is reported. */
        const val CONFIRM_SECONDS = 300L  // 5 minutes
        const val GEOFENCE_ID = "keystone.home"
        const val REQUEST_CODE = 0x4B45  // "KE"
    }

    private val geofencingClient: GeofencingClient =
        LocationServices.getGeofencingClient(context)

    private var radius = 150.0
    private var latitude = 0.0
    private var longitude = 0.0
    private var monitoring = false

    /** When the phone was last seen outside the zone; null while it is inside. */
    private var outsideSince: Long? = null
    /** What we last told the caller — so the same state is not reported twice. */
    private var lastReported: Boolean? = null

    private val innerRadius: Double get() = radius
    private val outerRadius: Double get() = radius * 1.5

    private val locationCallback = object : LocationCallback() {
        override fun onLocationResult(result: LocationResult) {
            val loc = result.lastLocation ?: return
            val distance = loc.distanceTo(android.location.Location(latitude, longitude))
            if (distance <= innerRadius) {
                outsideSince = null
                report(true)
            } else if (distance >= outerRadius) {
                if (outsideSince == null) outsideSince = System.currentTimeMillis()
                scheduleConfirmation()
            }
            // In the hysteresis band the state does not change — that is the
            // whole point of the band.
        }
    }

    /** Is the app allowed to see the location? */
    fun authorizationStatus(): String {
        val fine = ContextCompat.checkSelfPermission(context, Manifest.permission.ACCESS_FINE_LOCATION)
        val coarse = ContextCompat.checkSelfPermission(context, Manifest.permission.ACCESS_COARSE_LOCATION)
        return if (fine == PackageManager.PERMISSION_GRANTED ||
            coarse == PackageManager.PERMISSION_GRANTED
        ) "granted" else "notDetermined"
    }

    /** Start watching the zone. `radius` is in metres. */
    @SuppressLint("MissingPermission")
    fun start(latitude: Double, longitude: Double, radius: Double) {
        stop()
        this.latitude = latitude
        this.longitude = longitude
        this.radius = max(50.0, min(radius, 1000.0))

        if (authorizationStatus() != "granted") {
            onError("The app needs the location permission to notice when you arrive.")
            return
        }

        val geofence = Geofence.Builder()
            .setRequestId(GEOFENCE_ID)
            .setCircularRegion(latitude, longitude, outerRadius.toFloat())
            .setExpirationDuration(Long.MAX_VALUE)
            .setTransitionTypes(Geofence.GEOFENCE_TRANSITION_ENTER or Geofence.GEOFENCE_TRANSITION_EXIT)
            .setLoitering(false)
            .build()

        val request = GeofencingRequest.Builder()
            .setInitialTrigger(GeofencingRequest.INITIAL_TRIGGER_ENTER)
            .addGeofence(geofence)
            .build()

        geofencingClient.addGeofences(request, geofencePendingIntent())
            .addOnSuccessListener {
                monitoring = true
                // Where are we now? Without this the first "home" would only come
                // after the person walks out and back in.
                val locRequest = LocationRequest.Builder(
                    30_000L, com.google.android.gms.location.Priority.PRIORITY_BALANCED_POWER_ACCURACY, 50.0f
                ).setMinUpdateIntervalMillis(10_000L).build()
                LocationServices.getFusedLocationProviderClient(context)
                    .requestLocationUpdates(locRequest, locationCallback, null)
            }
            .addOnFailureListener { error ->
                onError(error.message ?: "Could not watch the region.")
            }
    }

    fun stop() {
        if (monitoring) {
            geofencingClient.removeGeofences(listOf(GEOFENCE_ID))
            monitoring = false
        }
        LocationServices.getFusedLocationProviderClient(context)
            .removeLocationUpdates(locationCallback)
        outsideSince = null
        lastReported = null
    }

    private fun geofencePendingIntent(): PendingIntent {
        val intent = Intent(context, GeofenceBroadcastReceiver::class.java)
        return PendingIntent.getBroadcast(
            context, REQUEST_CODE, intent,
            PendingIntent.FLAG_UPDATE_CURRENT or PendingIntent.FLAG_MUTABLE,
        )
    }

    /** Report "away" once the phone has been outside long enough. */
    private fun scheduleConfirmation() {
        android.os.Handler(android.os.Looper.getMainLooper()).postDelayed({
            val since = outsideSince ?: return@postDelayed
            if (System.currentTimeMillis() - since >= CONFIRM_SECONDS * 1000) {
                outsideSince = null
                report(false)
            }
        }, CONFIRM_SECONDS * 1000)
    }

    /** Tell the caller only when the state actually changed. */
    private fun report(home: Boolean) {
        if (lastReported == home) return
        lastReported = home
        onChange(home)
    }

    /**
     * The phone left the zone. Not "away" yet: it may only have stepped outside
     * for a moment, so the confirmation window decides.
     */
    fun onExit() {
        if (outsideSince == null) outsideSince = System.currentTimeMillis()
        scheduleConfirmation()
    }

    /**
     * The phone entered the zone. This one is immediate: coming home is the
     * event people actually want a light for.
     */
    fun onEnter() {
        outsideSince = null
        report(true)
    }
}
