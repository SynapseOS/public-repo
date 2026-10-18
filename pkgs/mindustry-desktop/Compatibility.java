/*
    Compatibility.java: Совместимость официального клиента Mindustry с SynapseOS
    Автор: Елчинян Арен Артурович 2026
    Архитектура: x86_64, ABI: Java Instrumentation и JNI musl

    Описание: Настройки клиента и исправления загрузки его библиотек
    Подсистема: пользовательская Java
    Первое изменение: 17.10.2026
    Последнее изменение: 18.10.2026
*/
package synapse;

import java.io.InputStream;
import java.lang.instrument.ClassFileTransformer;
import java.lang.instrument.Instrumentation;
import java.nio.file.Files;
import java.nio.file.Path;
import java.security.MessageDigest;
import java.security.ProtectionDomain;
import java.util.HashMap;
import java.util.HexFormat;
import java.util.Map;
import java.util.jar.JarFile;

public final class Compatibility {
	private static final String CLIENT_SHA256 =
	    "36d94941a1639478ad1d9be25a9d802c6f77552f400c6da0338b7b019f63a0d1";
	private static final String NATIVES = "/usr/share/games/mindustry/synapse-natives.jar";

	// Не создавать экземпляры служебного класса
	private Compatibility( ) {}

	// Проверять Main-Class, присутствие классов игры в чужом JAR недостаточно
	private static boolean isClient(String filename) {
		if (filename == null) {
			return false;
		}
		try (JarFile jar = new JarFile(filename)) {
			return jar.getManifest( ) != null &&
			    "mindustry.desktop.DesktopLauncher".equals(
			        jar.getManifest( ).getMainAttributes( ).getValue("Main-Class"));
		} catch (Exception exception) {
			return false;
		}
	}

	// Подключать проверенные исправления к закреплённой версии клиента
	public static void premain(String filename, Instrumentation instrumentation) {
		if (!isClient(filename)) {
			return;
		}
		try {
			MessageDigest digest = MessageDigest.getInstance("SHA-256");
			try (InputStream input = Files.newInputStream(Path.of(filename))) {
				byte[] buffer = new byte[32768];
				int count;
				while ((count = input.read(buffer)) != -1) {
					digest.update(buffer, 0, count);
				}
			}
			if (!CLIENT_SHA256.equals(HexFormat.of( ).formatHex(digest.digest( )))) {
				throw new IllegalArgumentException(
				    "Mindustry JAR differs from supported official v160.7");
			}
			if (!Files.isReadable(Path.of(NATIVES))) {
				throw new IllegalStateException("mindustry-desktop native libraries are missing");
			}
			Map<String, byte[]> replacements = new HashMap<>( );
			Path self = Path.of(Compatibility.class.getProtectionDomain( )
			                        .getCodeSource( )
			                        .getLocation( )
			                        .toURI( ));
			try (JarFile jar = new JarFile(self.toFile( ))) {
				var entries = jar.entries( );
				while (entries.hasMoreElements( )) {
					var entry = entries.nextElement( );
					String name = entry.getName( );
					if (name.startsWith("compat/") && name.endsWith(".class")) {
						try (InputStream input = jar.getInputStream(entry)) {
							replacements.put(name.substring(7, name.length( ) - 6),
							                 input.readAllBytes( ));
						}
					}
				}
			}
			if (!replacements.containsKey("arc/util/SharedLibraryLoader") ||
			    !replacements.containsKey("mindustry/net/Net")) {
				throw new IllegalStateException("Synapse Java compatibility module is incomplete");
			}
			instrumentation.addTransformer(new ClassFileTransformer( ) {
				// Исходные байты остальных классов остаются без изменений
				@Override
				public byte[] transform(ClassLoader loader, String name, Class<?> type,
				                        ProtectionDomain domain, byte[] bytes) {
					return name == null ? null : replacements.get(name);
				}
			});
			// Свойство выбирает Linux ABI для клиента, имя ОС и остальных JVM не меняется
			System.setProperty("os.name", "Linux");
			System.setProperty("SDL2_STATIC", "true");
			System.setProperty("nodiscord", "true");
			System.setProperty("synapse.mindustry.natives", NATIVES);
			System.setProperty("synapse.mindustry.compat", "160.7");
			if (System.getProperty("java.nio.channels.spi.SelectorProvider") == null) {
				System.setProperty("java.nio.channels.spi.SelectorProvider",
				                   "sun.nio.ch.PollSelectorProvider");
			}
		} catch (Exception exception) {
			System.err.println("SynapseOS Mindustry: " + exception.getMessage( ));
			System.exit(1);
		}
	}
}
