// TrustStore.java: воспроизводимый набор доверенных сертификатов JKS
// Автор: Елчинян Арен Артурович, 2026
// Подсистема: сборка OpenJDK
// Первое изменение: 18.10.2026, последнее изменение: 18.10.2026
// Статус доступа: общедоступный
import java.io.ByteArrayOutputStream;
import java.io.DataOutputStream;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.Path;
import java.security.MessageDigest;
import java.util.Base64;
import java.util.regex.Pattern;

public final class TrustStore {
	// Записать только публичные CA с постоянными именами и датой
	public static void main(String[] arguments) throws Exception {
		String pem = Files.readString(Path.of(arguments[0]));
		var matches = Pattern
		                  .compile("-----BEGIN CERTIFICATE-----(.*?)-----END CERTIFICATE-----",
		                           Pattern.DOTALL)
		                  .matcher(pem)
		                  .results( )
		                  .toList( );
		ByteArrayOutputStream bytes = new ByteArrayOutputStream( );
		DataOutputStream output = new DataOutputStream(bytes);
		output.writeInt(0xfeedfeed);
		output.writeInt(2);
		output.writeInt(matches.size( ));
		int index = 0;
		for (var match : matches) {
			byte[] certificate = Base64.getMimeDecoder( ).decode(match.group(1));
			output.writeInt(2);
			output.writeUTF("synapse-ca-" + index++);
			output.writeLong(0);
			output.writeUTF("X.509");
			output.writeInt(certificate.length);
			output.write(certificate);
		}
		output.flush( );
		MessageDigest digest = MessageDigest.getInstance("SHA-1");
		digest.update("changeit".getBytes(StandardCharsets.UTF_16BE));
		digest.update("Mighty Aphrodite".getBytes(StandardCharsets.US_ASCII));
		digest.update(bytes.toByteArray( ));
		output.write(digest.digest( ));
		Files.write(Path.of(arguments[1]), bytes.toByteArray( ));
	}
}
